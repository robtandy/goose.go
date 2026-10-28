package goose

import (
	"math"
	"runtime"
	"runtime/debug"
	"sync"
	"time"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Workers and typed queues (goose_spec.md §11.2), as src/runtime/
// runtime_threads.h has them and the Go backend's runtime prologue
// (src/codegen_go_runtime.h, go_runtime_threads) writes them into a
// generated file: keep the two the same.
//
// A worker is a goroutine running its thread program on a GsStackBlock of
// its own, with a program instance of its own: the globals its program
// uses, copied from the spawning instance at the spawn, with its arguments
// (GsThreadSpawn). A queue is a FIFO of the images of the values put, under
// a mutex (GsQueue). This is the one part of the runtime that allocates on
// Go's heap: what a queue holds, and a worker's record.

// The program's address space budget for regions (GS_STACK_BUDGET), the
// most regions its main program and any one worker hold, and the workers
// that leaves room for beside the main program: what hardware_threads()
// reports at most (gs_rt_start).
var (
	gsStackBudget   uint64
	gsMainRegions   int64
	gsWorkerRegions int64
	gsThreadCap     int64 = math.MaxInt64
)

// GsRtStart is the start of the runtime, as main's thread program begins
// (gs_rt_start): the budget, and the cap on the workers it holds, each at
// the most regions one can take, at least one. Workers that hold no
// regions leave the cap off. reserve and gap are each region's sizes.
func GsRtStart(budget uint64, reserve, gap, mainregions, workerregions int64) {
	gsStackBudget, gsMainRegions, gsWorkerRegions = budget, mainregions, workerregions
	if workerregions <= 0 {
		gsThreadCap = math.MaxInt64
		return
	}
	per := uint64(reserve-reserve%gsPageSize) + uint64(gap)
	total := uint64(0)
	if per != 0 {
		total = budget / per
	}
	spare := int64(math.MaxInt64)
	if total <= math.MaxInt64 {
		spare = int64(total)
	}
	spare -= mainregions
	if spare > workerregions {
		gsThreadCap = spare / workerregions
	} else {
		gsThreadCap = 1
	}
}

// GsHardwareThreads is hardware_threads(): the processors Go runs
// goroutines on at once, but no more than the workers the data stack
// budget holds beside the main program (GsRtStart), so that a pool sized by
// it never runs the program out of address space.
func GsHardwareThreads() int64 {
	n := int64(runtime.GOMAXPROCS(0))
	if n > gsThreadCap {
		n = gsThreadCap
	}
	return n
}

// The workers (gs_threads): ids increase from 0 and are never reused, and
// only a running worker has a record, the channel its end closes. Of those
// that ended, the runtime keeps a bit for each contained worker that
// aborted, from which GsThreadWait answers, and the message of each such
// abort until a wait takes it.
var gsThreads struct {
	mu      sync.Mutex
	next    int64
	running map[int64]chan struct{}
	aborted []uint64
	msgs    map[int64]string
}

// GsThreadSpawn starts a worker running run on a block of its own and
// returns its id (gs_thread_start): `stacks` data stacks of the default
// sizes, which the worker reserves as it starts, a program instance of
// glsize zeroed bytes at its Gl, and a copy of the n-byte spawn image at
// args, taken now, which run is handed and the worker owns, outside the Go
// heap as the stacks are. In a contained worker an
// abort (a *GsError with a location) ends run early, its message kept for
// GsThreadWait; anything else that panics out of run ends the process, as
// on main's thread. Whichever way it ends, the worker releases all it holds
// before GsThreadWait sees it ended.
func GsThreadSpawn(run func(gs *GsStackBlock, p unsafe.Pointer), stacks int, glsize uintptr,
	args unsafe.Pointer, n int64, contained bool) int64 {
	if n < 0 {
		GsPanic("invalid thread argument size")
	}
	// The instance and the image, in one mapping the worker's block owns.
	off := (int64(glsize) + 7) &^ 7
	inst := gsMapPrivate(off + n)
	p := unsafe.Add(unsafe.Pointer(unsafe.SliceData(inst)), off)
	copy(unsafe.Slice((*byte)(p), n), unsafe.Slice((*byte)(args), n))
	done := make(chan struct{})
	gsThreads.mu.Lock()
	if gsThreads.next == math.MaxInt64 {
		gsThreads.mu.Unlock()
		GsPanic("thread id space exhausted")
	}
	id := gsThreads.next
	gsThreads.next++
	if gsThreads.running == nil {
		gsThreads.running = map[int64]chan struct{}{}
	}
	gsThreads.running[id] = done
	gsThreads.mu.Unlock()
	go gsThreadMain(inst, stacks, run, p, contained, id, done)
	return id
}

// gsThreadMain is a worker's goroutine (gs_thread_main): its block, whose
// stacks it reserves as it starts (gs_stack_block), its thread program,
// then the release of everything it holds, and only then its end
// published.
func gsThreadMain(inst []byte, stacks int, run func(*GsStackBlock, unsafe.Pointer), p unsafe.Pointer,
	contained bool, id int64, done chan struct{}) {
	// A fault in a guard gap reaches the worker's recover, as main's does.
	debug.SetPanicOnFault(true)
	var gs *GsStackBlock
	msg, aborted := gsRunWorker(func() {
		gs = NewStackBlock(stacks, GsStackReserve, GsStackGap)
		gs.inst, gs.Gl, gs.worker = inst, unsafe.Pointer(unsafe.SliceData(inst)), id+1
		run(gs, p)
	}, contained)
	gs.Close()
	gsThreads.mu.Lock()
	delete(gsThreads.running, id)
	if aborted {
		for int64(len(gsThreads.aborted)) <= id/64 {
			gsThreads.aborted = append(gsThreads.aborted, 0)
		}
		gsThreads.aborted[id/64] |= 1 << uint(id%64)
		if gsThreads.msgs == nil {
			gsThreads.msgs = map[int64]string{}
		}
		gsThreads.msgs[id] = msg
	}
	close(done)
	gsThreads.mu.Unlock()
}

// gsRunWorker runs a worker's thread program, f. An abort in a contained one
// returns here, with its message (gs_run_contained); anything else ends the
// process, as it does on main's (gsEndOnPanic): an abort in a worker that is
// not contained, and the runtime's own failures, which have no location in
// the program.
func gsRunWorker(f func(), contained bool) (msg string, aborted bool) {
	defer func() {
		if r := recover(); r != nil {
			if e, ok := r.(*GsError); ok && contained && e.File != "" {
				msg, aborted = e.Text(), true
				return
			}
			gsEndOnPanic(r)
		}
	}()
	f()
	return "", false
}

// GsThreadWait waits until the worker id has ended and released all it
// held (gs_thread_wait): false for a contained worker that aborted. With
// take, the first such wait for that worker takes the abort's message,
// where later ones, and any for a worker that returned, get "". gs is the
// block of the thread program waiting, whose worker cannot wait for itself;
// an id never issued aborts too.
func GsThreadWait(gs *GsStackBlock, id int64, take bool, file string, line int) (bool, string) {
	gsThreads.mu.Lock()
	if id < 0 || id >= gsThreads.next {
		gsThreads.mu.Unlock()
		GsAbort(GsEThreadID, file, line)
	}
	if id == gs.worker-1 {
		gsThreads.mu.Unlock()
		GsAbort(GsEThreadSelf, file, line)
	}
	done := gsThreads.running[id]
	gsThreads.mu.Unlock()
	if done != nil {
		<-done
	}
	gsThreads.mu.Lock()
	defer gsThreads.mu.Unlock()
	if id/64 >= int64(len(gsThreads.aborted)) || gsThreads.aborted[id/64]>>uint(id%64)&1 == 0 {
		return true, ""
	}
	if !take {
		return false, ""
	}
	m := gsThreads.msgs[id]
	delete(gsThreads.msgs, id)
	return false, m
}

// GsKeep is s, or where it views memory of gs's own -- a global of the
// program instance a call of exported Goose code ran on -- a copy of it on
// Go's heap: the result crosses to its caller once the call's block is back
// in the pool, where the next call takes its memory.
func GsKeep[T any](gs *GsStackBlock, s []T) []T {
	if len(s) == 0 || !gs.owns(unsafe.Pointer(unsafe.SliceData(s))) {
		return s
	}
	return append([]T(nil), s...)
}

// GsQueue is the queue of one element type (goose_spec.md §11.2): the
// image of each value put, in order, and how many there are. The zero
// GsQueue is an empty one with no limit, which is how a program declares
// each of its queues; it must not be copied once used.
type GsQueue struct {
	mu      sync.Mutex
	head    *gsQNode
	tail    *gsQNode
	count   int64
	limit   int64           // 0 until GsQLimit gives one.
	waiters []chan struct{} // The receivers waiting, the first first: a put wakes one.
}

type gsQNode struct {
	next *gsQNode
	b    []byte
}

// GsQPut queues a copy of the n bytes at p (gs_qput), taken before the lock
// is: false, queueing nothing, where the queue holds its limit of messages.
// It never waits. A value of no bytes is a message of none.
func GsQPut(q *GsQueue, p unsafe.Pointer, n int64) bool {
	nd := &gsQNode{b: make([]byte, n)}
	copy(nd.b, unsafe.Slice((*byte)(p), n))
	q.mu.Lock()
	if q.limit > 0 && q.count >= q.limit {
		q.mu.Unlock()
		return false
	}
	q.count++
	if q.tail != nil {
		q.tail.next = nd
	} else {
		q.head = nd
	}
	q.tail = nd
	if len(q.waiters) > 0 {
		close(q.waiters[0])
		q.unwait(q.waiters[0])
	}
	q.mu.Unlock()
	return true
}

// GsQPutV queues the image of v, a fixed-size value, n bytes of it.
func GsQPutV[T any](q *GsQueue, v T, n int64) bool { return GsQPut(q, unsafe.Pointer(&v), n) }

// GsQValue is the fixed-size value of type T whose image b is: all zero
// bytes past what b holds, all of them for a value of no bytes or a miss.
func GsQValue[T any](b []byte) (v T) {
	copy(unsafe.Slice((*byte)(unsafe.Pointer(&v)), unsafe.Sizeof(v)), b)
	return
}

// take unlinks the first message, under the lock.
func (q *GsQueue) take() []byte {
	nd := q.head
	q.head = nd.next
	if q.head == nil {
		q.tail = nil
	}
	q.count--
	return nd.b
}

// unwait drops the receiver w from those waiting, where a put has not
// already, under the lock.
func (q *GsQueue) unwait(w chan struct{}) {
	for i, x := range q.waiters {
		if x == w {
			copy(q.waiters[i:], q.waiters[i+1:])
			q.waiters[len(q.waiters)-1] = nil
			q.waiters = q.waiters[:len(q.waiters)-1]
			return
		}
	}
}

// GsQGet takes the first message, waiting for one (gs_qget).
func GsQGet(q *GsQueue) []byte {
	q.mu.Lock()
	for q.head == nil {
		w := make(chan struct{})
		q.waiters = append(q.waiters, w)
		q.mu.Unlock()
		<-w
		q.mu.Lock()
	}
	b := q.take()
	q.mu.Unlock()
	return b
}

// GsQWait takes the first message, waiting at most ms milliseconds for one,
// on a monotonic clock (gs_qwait): false where none came. Zero or less does
// not wait, which is qpoll (gs_qpoll).
func GsQWait(q *GsQueue, ms int64) ([]byte, bool) {
	// The deadline in nanoseconds cannot overflow.
	if ms > math.MaxInt64/2000000 {
		ms = math.MaxInt64 / 2000000
	}
	var deadline time.Time
	if ms > 0 {
		deadline = time.Now().Add(time.Duration(ms) * time.Millisecond)
	}
	q.mu.Lock()
	for q.head == nil && ms > 0 {
		left := time.Until(deadline)
		if left <= 0 {
			break
		}
		w := make(chan struct{})
		q.waiters = append(q.waiters, w)
		q.mu.Unlock()
		t := time.NewTimer(left)
		select {
		case <-w:
		case <-t.C:
		}
		t.Stop()
		q.mu.Lock()
		q.unwait(w)
	}
	if q.head == nil {
		q.mu.Unlock()
		return nil, false
	}
	b := q.take()
	q.mu.Unlock()
	return b, true
}

// GsQLimit gives the queue a limit of n messages (gs_qlimit): one below what
// it holds keeps them, and puts fail until fewer remain. A limit of zero or
// less aborts.
func GsQLimit(q *GsQueue, n int64, file string, line int) {
	if n <= 0 {
		GsAbort(GsEQBound, file, line)
	}
	q.mu.Lock()
	q.limit = n
	q.mu.Unlock()
}
