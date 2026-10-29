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
// Workers and typed queues (§11.2), as runtime_threads.h has them. A worker
// is a goroutine running its thread program on a block of its own, with a
// program instance of its own: the globals its program uses, copied from
// the spawning instance at the spawn with its arguments (GsThreadSpawn). A
// queue is a FIFO of the images of the values put, under a mutex (GsQueue).
// This is the one part of the runtime that allocates on Go's heap: what a
// queue holds, and a worker's record.

// HardwareThreads is hardware_threads(): the processors Go runs goroutines
// on at once, but no more than the workers the program's data stack budget
// holds beside its main program (Start), so that a pool sized by it never
// runs the program out of address space.
func (p *GsProgram) HardwareThreads() int64 {
	n := int64(runtime.GOMAXPROCS(0))
	if p.threadCap > 0 && n > p.threadCap {
		n = p.threadCap
	}
	return n
}

// The workers (gs_threads): ids increase from 0 and are never reused, and
// only a running worker has a record, the channel its end closes. Of those
// that ended, the runtime keeps a bit for each contained worker that
// aborted, from which thread_wait answers, and the message of each such
// abort until a wait takes it.
var gsThreads struct {
	mu      sync.Mutex
	next    int64
	running map[int64]chan struct{}
	aborted []uint64
	msgs    map[int64]string
}

// ThreadSpawn starts a worker of the program running run on a block of its
// own and returns its id (gs_thread_start): `stacks` data stacks of the
// program's sizes, which the worker reserves as it starts, a program
// instance of glsize zeroed bytes at its Gl, and a copy of the n-byte spawn
// image at args, taken now, which run is handed and the worker owns,
// outside the Go heap as the stacks are. In a contained worker an abort
// ends run early, its message kept for thread_wait (§11.2). Whichever way
// it ends, the worker releases all it holds before thread_wait sees it
// ended.
func (p *GsProgram) ThreadSpawn(run func(gs *GsStackBlock, p unsafe.Pointer), stacks int, glsize uintptr,
	args unsafe.Pointer, n int64, contained bool) int64 {
	if n < 0 {
		GsPanic("invalid thread argument size")
	}
	// The instance and the image, in one mapping the worker's block owns.
	off := (int64(glsize) + 7) &^ 7
	inst := gsMapPrivate(off + n)
	img := unsafe.Add(unsafe.Pointer(unsafe.SliceData(inst)), off)
	copy(unsafe.Slice((*byte)(img), n), unsafe.Slice((*byte)(args), n))
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
	go gsThreadMain(p, inst, stacks, run, img, contained, id, done)
	return id
}

// gsThreadMain is a worker's goroutine (gs_thread_main): its block, whose
// stacks it reserves as it starts (gs_stack_block), its thread program,
// then the release of everything it holds, and only then its end
// published.
func gsThreadMain(p *GsProgram, inst []byte, stacks int, run func(*GsStackBlock, unsafe.Pointer),
	img unsafe.Pointer, contained bool, id int64, done chan struct{}) {
	// A fault in a guard gap reaches the worker's recover, as main's does.
	debug.SetPanicOnFault(true)
	var gs *GsStackBlock
	msg, aborted := gsRunWorker(func() {
		gs = p.NewStackBlock(stacks, p.StackReserve, p.StackGap)
		gs.inst, gs.Gl, gs.worker = inst, unsafe.Pointer(unsafe.SliceData(inst)), id+1
		run(gs, img)
	}, contained, p.Library)
	_ = gs.Close()
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
// not contained, the runtime's own failures, which have no location in the
// program, a fault in a guard gap. A library's worker ends no process: what
// it panics with goes on.
func gsRunWorker(f func(), contained, library bool) (msg string, aborted bool) {
	defer func() {
		if r := recover(); r != nil {
			if e, ok := r.(*GsError); ok && contained && e.File != "" {
				msg, aborted = e.Text(), true
				return
			}
			gsEndOnPanic(r, library)
		}
	}()
	f()
	return "", false
}

// GsThreadWait waits until the worker id has ended and released all it held
// (gs_thread_wait): false for a contained worker that aborted. With take,
// the first such wait for that worker takes the abort's message, where
// later ones, and any for a worker that returned, get "". gs is the block
// of the thread program waiting.
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
// program instance a call of an export fn ran on -- a copy of it on Go's
// heap: an export's result crosses to its caller once the call's block is
// back in the pool, where the next call takes its memory.
func GsKeep[T any](gs *GsStackBlock, s []T) []T {
	if len(s) == 0 || !gs.Owns(unsafe.Pointer(unsafe.SliceData(s))) {
		return s
	}
	return append([]T(nil), s...)
}

// GsQueue is the queue of one element type (§11.2): the image of each value
// put, in order, and how many there are. The zero GsQueue is an empty one
// with no limit, which is how the program declares each of its queues.
type GsQueue struct {
	mu      sync.Mutex
	head    *gsQNode
	tail    *gsQNode
	count   int64
	limit   int64           // 0 until qlimit gives one.
	waiters []chan struct{} // The receivers waiting, the first first: a put wakes one.
}

type gsQNode struct {
	next *gsQNode
	b    []byte
}

// GsQPut queues a copy of the n bytes at p (gs_qput), taken before the lock
// is: false, queueing nothing, where the queue holds its limit of messages.
// It never waits.
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
