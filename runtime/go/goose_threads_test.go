package goose

import (
	"math"
	"sync"
	"testing"
	"time"
	"unsafe"
)

// The queues and workers of goose_threads.go, as the Go backend's generated
// code uses them (src/codegen_go_threads.h).

func TestQueueOrderLimitAndPoll(t *testing.T) {
	var q GsQueue
	for i := int64(1); i <= 3; i++ {
		if !GsQPutV(&q, i*10, 8) {
			t.Fatalf("put %d refused with no limit", i)
		}
	}
	if b, ok := GsQWait(&q, 0); !ok || GsQValue[int64](b) != 10 {
		t.Fatalf("poll = %v, %v", b, ok)
	}
	// A lower limit keeps what is queued; puts fail until fewer remain.
	GsQLimit(&q, 1, "q.goose", 1)
	if GsQPutV(&q, int64(40), 8) {
		t.Fatalf("a put past the limit was queued")
	}
	if got := GsQValue[int64](GsQGet(&q)); got != 20 {
		t.Fatalf("get = %d, want 20", got)
	}
	if GsQPutV(&q, int64(40), 8) {
		t.Fatalf("a put at the limit was queued")
	}
	if got := GsQValue[int64](GsQGet(&q)); got != 30 {
		t.Fatalf("get = %d, want 30", got)
	}
	if !GsQPutV(&q, int64(50), 8) {
		t.Fatalf("a put below the limit was refused")
	}
	if got := GsQValue[int64](GsQGet(&q)); got != 50 {
		t.Fatalf("get = %d", got)
	}
	// A miss yields nothing, which is the type's zero value.
	b, ok := GsQWait(&q, 0)
	if ok || b != nil || GsQValue[int64](b) != 0 {
		t.Fatalf("missed poll = %v, %v", b, ok)
	}
	// A value of no bytes is a message of none.
	var z GsQueue
	if !GsQPut(&z, nil, 0) {
		t.Fatalf("a 0-byte put was refused")
	}
	if b, ok := GsQWait(&z, 0); !ok || len(b) != 0 {
		t.Fatalf("0-byte poll = %v, %v", b, ok)
	}
	mustAbort(t, "goose runtime error: queue limit of zero or less (q.goose:7)", func() { GsQLimit(&q, 0, "q.goose", 7) })
	mustAbort(t, "goose runtime error: queue limit of zero or less (q.goose:8)", func() { GsQLimit(&q, -1, "q.goose", 8) })
}

func TestQueueWaits(t *testing.T) {
	var q GsQueue
	// A wait with nothing queued ends after its time, on a monotonic clock;
	// none of zero or less waits.
	t0 := time.Now()
	if _, ok := GsQWait(&q, 30); ok {
		t.Fatalf("a wait on an empty queue got a value")
	}
	if d := time.Since(t0); d < 25*time.Millisecond || d > 5*time.Second {
		t.Fatalf("a 30 ms wait took %v", d)
	}
	t0 = time.Now()
	if _, ok := GsQWait(&q, -5); ok {
		t.Fatalf("a wait of less than nothing got a value")
	}
	if d := time.Since(t0); d > time.Second {
		t.Fatalf("a wait of less than nothing took %v", d)
	}
	// The longest wait there is, its deadline capped, for a value queued.
	GsQPutV(&q, int64(6), 8)
	if b, ok := GsQWait(&q, math.MaxInt64); !ok || GsQValue[int64](b) != 6 {
		t.Fatalf("the longest wait = %v, %v", b, ok)
	}
	// A value another goroutine puts before the time is up, and one a
	// blocking get waits for.
	go func() {
		time.Sleep(10 * time.Millisecond)
		GsQPutV(&q, int64(7), 8)
		time.Sleep(10 * time.Millisecond)
		GsQPutV(&q, int64(8), 8)
	}()
	if b, ok := GsQWait(&q, 60000); !ok || GsQValue[int64](b) != 7 {
		t.Fatalf("timed get = %v, %v", b, ok)
	}
	if got := GsQValue[int64](GsQGet(&q)); got != 8 {
		t.Fatalf("get = %d", got)
	}
	if len(q.waiters) != 0 {
		t.Fatalf("%d receivers left waiting", len(q.waiters))
	}
}

// Producers and consumers at once, some of the consumers timed: every value
// put is received once.
func TestQueueConcurrent(t *testing.T) {
	var q GsQueue
	GsQLimit(&q, 16, "", 0)
	const producers, each = 4, 500
	var wg sync.WaitGroup
	for p := 0; p < producers; p++ {
		wg.Add(1)
		go func(p int64) {
			defer wg.Done()
			for i := int64(0); i < each; i++ {
				for !GsQPutV(&q, p*each+i, 8) {
					time.Sleep(time.Microsecond)
				}
			}
		}(int64(p))
	}
	seen := make([]bool, producers*each)
	var mu sync.Mutex
	var cw sync.WaitGroup
	for c := 0; c < 3; c++ {
		cw.Add(1)
		go func(c int) {
			defer cw.Done()
			for {
				var b []byte
				if c == 0 {
					var ok bool
					if b, ok = GsQWait(&q, 200); !ok {
						return
					}
				} else {
					b = GsQGet(&q)
				}
				v := GsQValue[int64](b)
				if v < 0 {
					return
				}
				mu.Lock()
				if seen[v] {
					t.Errorf("%d received twice", v)
				}
				seen[v] = true
				mu.Unlock()
			}
		}(c)
	}
	wg.Wait()
	// A stop for each consumer, which the timed one may end before it
	// takes one of.
	for c := 0; c < 3; c++ {
		for !GsQPutV(&q, int64(-1), 8) {
			time.Sleep(time.Microsecond)
		}
	}
	cw.Wait()
	for v, ok := range seen {
		if !ok {
			t.Fatalf("%d never received", v)
		}
	}
}

// A worker's spawn image: an i64 its thread program reads.
func spawnI64(run func(*GsStackBlock, unsafe.Pointer), v int64, contained bool) int64 {
	return GsThreadSpawn(run, 2, 64, unsafe.Pointer(&v), 8, contained)
}

func TestWorkers(t *testing.T) {
	main := NewStackBlock(1, GsStackReserve, GsStackGap)
	defer main.Close()
	var results GsQueue
	blocks := make(chan *GsStackBlock, 4)
	run := func(gs *GsStackBlock, p unsafe.Pointer) {
		// The block is the worker's own: its stacks, a zeroed instance and
		// the copy of the image, which outlives the spawner's.
		blocks <- gs
		if len(gs.Stks) != 2 || gs.Gl == nil || *(*int64)(gs.Gl) != 0 {
			GsPanic("not a worker's block")
		}
		*(*int64)(gs.Gl) = 1
		GsQPutV(&results, *(*int64)(p)*2, 8)
	}
	a := spawnI64(run, 21, false)
	b := spawnI64(run, 4, false)
	if b != a+1 {
		t.Fatalf("ids %d then %d", a, b)
	}
	sum := GsQValue[int64](GsQGet(&results)) + GsQValue[int64](GsQGet(&results))
	if sum != 50 {
		t.Fatalf("results sum to %d", sum)
	}
	for _, id := range []int64{a, b, a} {
		if ok, msg := GsThreadWait(main, id, true, "w.goose", 1); !ok || msg != "" {
			t.Fatalf("wait %d = %v, %q", id, ok, msg)
		}
	}
	// Each worker released its block before its wait returned.
	for i := 0; i < 2; i++ {
		if gs := <-blocks; len(gs.Stks) != 0 || gs.Gl != nil || gs.inst != nil {
			t.Fatalf("a worker's block outlived its end")
		}
	}
	mustAbort(t, "goose runtime error: thread_wait on an unknown thread id (w.goose:2)", func() {
		GsThreadWait(main, b+100, false, "w.goose", 2)
	})
	mustAbort(t, "goose runtime error: thread_wait on an unknown thread id (w.goose:3)", func() {
		GsThreadWait(main, -1, false, "w.goose", 3)
	})
}

func TestContainedWorkers(t *testing.T) {
	main := NewStackBlock(0, GsStackReserve, GsStackGap)
	defer main.Close()
	// An abort ends only its worker; the first wait that asks takes the
	// message, which ends with the abort's location.
	id := spawnI64(func(gs *GsStackBlock, p unsafe.Pointer) {
		GsAbort(GsEAssert, "c.goose", int(*(*int64)(p)))
	}, 12, true)
	if ok, msg := GsThreadWait(main, id, false, "", 0); ok || msg != "" {
		t.Fatalf("a wait that does not ask = %v, %q", ok, msg)
	}
	if ok, msg := GsThreadWait(main, id, true, "", 0); ok || msg != "assert failed (c.goose:12)" {
		t.Fatalf("the first wait that asks = %v, %q", ok, msg)
	}
	if ok, msg := GsThreadWait(main, id, true, "", 0); ok || msg != "" {
		t.Fatalf("a later wait = %v, %q", ok, msg)
	}
	// A worker waiting for itself is an abort, which is contained here.
	self := spawnI64(func(gs *GsStackBlock, p unsafe.Pointer) {
		GsThreadWait(gs, gs.worker-1, false, "c.goose", 20)
	}, 0, true)
	if ok, msg := GsThreadWait(main, self, true, "", 0); ok || msg != "thread_wait on the current thread (c.goose:20)" {
		t.Fatalf("self wait = %v, %q", ok, msg)
	}
	// One that returns answers true, with no message.
	ret := spawnI64(func(gs *GsStackBlock, p unsafe.Pointer) {}, 0, true)
	if ok, msg := GsThreadWait(main, ret, true, "", 0); !ok || msg != "" {
		t.Fatalf("a returning contained worker = %v, %q", ok, msg)
	}
	// Several waiters for one worker all see its end.
	gate := make(chan struct{})
	slow := spawnI64(func(gs *GsStackBlock, p unsafe.Pointer) {
		<-gate
		GsAbort(GsEPop, "c.goose", 30)
	}, 0, true)
	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if ok, _ := GsThreadWait(main, slow, false, "", 0); ok {
				t.Errorf("a waiter saw the aborted worker return")
			}
		}()
	}
	close(gate)
	wg.Wait()
}

func TestHardwareThreads(t *testing.T) {
	defer GsRtStart(0, GsStackReserve, GsStackGap, 0, 0)
	if n := GsHardwareThreads(); n < 1 {
		t.Fatalf("hardware threads %d", n)
	}
	// A 4 GB budget holds two regions of the default reservation, fewer
	// than any program with a worker takes: the cap is its floor, one.
	GsRtStart(4<<30, GsStackReserve, GsStackGap, 2, 3)
	if n := GsHardwareThreads(); n != 1 {
		t.Fatalf("capped hardware threads %d, want 1", n)
	}
	// Workers that hold no regions leave it uncapped.
	GsRtStart(4<<30, GsStackReserve, GsStackGap, 2, 0)
	if gsThreadCap != math.MaxInt64 {
		t.Fatalf("cap %d with no worker regions", gsThreadCap)
	}
}

func TestInstancesAndKeep(t *testing.T) {
	gs := NewStackBlock(1, GsStackReserve, GsStackGap)
	defer gs.Close()
	gs.NewInstance(24)
	gl := gs.Gl
	*(*int64)(gl) = 5
	gs.NewInstance(24)
	if gs.Gl != gl || *(*int64)(gl) != 5 {
		t.Fatalf("a second NewInstance replaced the instance")
	}
	// Reset gives the instance up; the next one reuses its memory, zeroed.
	gs.Reset()
	if gs.Gl != nil {
		t.Fatalf("Reset kept the instance")
	}
	gs.NewInstance(24)
	if gs.Gl != gl || *(*int64)(gs.Gl) != 0 {
		t.Fatalf("the instance after Reset is not a zeroed reuse")
	}
	// A result viewing the block's memory crosses as a copy; any other as
	// it is.
	view := unsafe.Slice((*byte)(gs.Gl), 8)
	if kept := GsKeep(gs, view); &kept[0] == &view[0] || kept[0] != 0 {
		t.Fatalf("GsKeep kept a view of the block's instance")
	}
	stack := unsafe.Slice((*byte)(gs.Stks[0]), 4)
	if kept := GsKeep(gs, stack); &kept[0] == &stack[0] {
		t.Fatalf("GsKeep kept a view of the block's stack")
	}
	own := []byte("caller's")
	if kept := GsKeep(gs, own); &kept[0] != &own[0] {
		t.Fatalf("GsKeep copied the caller's memory")
	}
}
