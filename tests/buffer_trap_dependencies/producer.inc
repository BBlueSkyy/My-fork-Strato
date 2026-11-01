// Production methods select the writer cycle; these gates model pending readers.
static void AttachProducerAndReader(Fixture &f, const std::shared_ptr<gpu::FenceCycle> &producer,
                                    const std::shared_ptr<gpu::FenceCycle> &reader) {
    auto b = f.buffer;
    {
        std::scoped_lock lock{*b};
        b->MarkGpuDirtyImpl();
        b->UpdateCycle(producer);
    }
    {
        std::scoped_lock lock{*b};
        b->UpdateCycle(reader);
    }
}
static void ProducerReadbackDoesNotWaitForReader() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    producer->complete = [&] { std::memset(b->backing->data(), 0x64, 4096); };
    auto reader = std::make_shared<gpu::FenceCycle>();
    std::promise<void> release;
    auto gate = release.get_future().share();
    reader->complete = [gate] { gate.wait(); };
    AttachProducerAndReader(f, producer, reader);
    auto store = std::async(std::launch::async, [&] { f.Write(37, 0x9a); });
    const bool independent = store.wait_for(200ms) == std::future_status::ready;
    if (!independent) {
        release.set_value();
        store.get();
        Check(false, "GPU-dirty readback waited for a later read-only consumer");
    }
    store.get();
    Check(producer->done && reader->waits == 0 && b->cycle == reader, "readback discarded latest-use dependency");
    for (size_t i{}; i < 4096; ++i)
        Check(f.mirror[i] == (i == 37 ? 0x9a : 0x64), "producer readback lost partial-store bytes");
    Check(b->backing->data()[37] == 0x64, "CPU store changed backing of pending reader");
    auto upload = std::async(std::launch::async, [&] {
        std::scoped_lock lock{*b};
        b->SynchronizeHost();
    });
    const bool retained = upload.wait_for(100ms) == std::future_status::timeout;
    release.set_value();
    upload.get();
    Check(retained && reader->done, "upload omitted pending-reader dependency");
    for (size_t i{}; i < 4096; ++i)
        Check(b->backing->data()[i] == (i == 37 ? 0x9a : 0x64), "upload corrupted preserved bytes");
    std::puts(
        "PASS GPU-dirty producer readback overlaps later reader; full upload still waits and preserves all bytes");
}
static void FailedTrackedProducerRetries() {
    Fixture f;
    auto b = f.buffer;
    auto p = std::make_shared<gpu::FenceCycle>();
    auto r = std::make_shared<gpu::FenceCycle>();
    p->complete = [] { throw std::runtime_error("failed GPU producer"); };
    AttachProducerAndReader(f, p, r);
    bool failed{};
    try {
        f.process.trap.HandleTrap(f.guest, true);
    } catch (const std::runtime_error &) {
        failed = true;
    }
    Check(failed && b->gpuWriteCycle == p && b->cycle == r && r->waits == 0 &&
              b->dirtyState == Buffer::DirtyState::GpuDirty,
          "failed tracked producer discarded data authority or pending-reader lifetime");
    p->complete = [&] { std::memset(b->backing->data(), 0x64, 4096); };
    Check(f.process.trap.HandleTrap(f.guest, true), "tracked producer retry failed");
    f.Write(37, 0x9a);
    Check(f.mirror[0] == 0x64 && f.mirror[37] == 0x9a && b->cycle == r, "tracked recovery lost data/readers");
    std::puts("PASS failed tracked producer retains state/cycles; retry restores current data");
}
static void NewWriterDuringWaitIsRevalidated() {
    Fixture f;
    auto b = f.buffer;
    auto p = std::make_shared<gpu::FenceCycle>();
    auto r = std::make_shared<gpu::FenceCycle>();
    std::promise<void> firstEntered, releaseFirst, secondEntered, releaseSecond;
    auto firstGate = releaseFirst.get_future().share(), secondGate = releaseSecond.get_future().share();
    p->complete = [&] {
        firstEntered.set_value();
        firstGate.wait();
        std::memset(b->backing->data(), 0x64, 4096);
    };
    AttachProducerAndReader(f, p, r);
    auto store = std::async(std::launch::async, [&] { f.Write(37, 0x9a); });
    firstEntered.get_future().wait();
    auto next = std::make_shared<gpu::FenceCycle>();
    next->complete = [&] {
        secondEntered.set_value();
        secondGate.wait();
        std::memset(b->backing->data(), 0x42, 4096);
    };
    {
        std::scoped_lock lock{*b};
        b->MarkGpuDirtyImpl();
        b->UpdateCycle(next);
    }
    releaseFirst.set_value();
    secondEntered.get_future().wait();
    const bool retained = store.wait_for(100ms) == std::future_status::timeout;
    releaseSecond.set_value();
    store.get();
    Check(retained && next->done, "new producer was skipped after outside-lock wait");
    for (size_t i{}; i < 4096; ++i)
        Check(f.mirror[i] == (i == 37 ? 0x9a : 0x42), "new writer retry read stale bytes");
    std::puts("PASS concurrent newer writer is revalidated after waiting; latest bytes preserved");
}
static void FullPollAndAllWritesRemainConservative() {
    Fixture f;
    auto b = f.buffer;
    auto p = std::make_shared<gpu::FenceCycle>();
    auto r = std::make_shared<gpu::FenceCycle>();
    p->complete = [&] { std::memset(b->backing->data(), 0x64, 4096); };
    AttachProducerAndReader(f, p, r);
    p->Wait();
    {
        std::scoped_lock lock{*b};
        b->BlockAllCpuBackingWrites();
        Check(!b->SynchronizeGuest(true, true), "AllWrites bypassed the full dependency");
    }
    {
        std::scoped_lock lock{*b};
        Check(!b->PollFence() && b->gpuWriteCycle == p, "pending full poll discarded producer");
        Check(b->SynchronizeGuest(true, true) && b->cycle == r,
              "nonblocking ready producer unnecessarily waited for reader");
    }
    r->Wait();
    {
        std::scoped_lock lock{*b};
        Check(b->PollFence() && !b->cycle && !b->gpuWriteCycle, "completed full poll retained stale cycles");
    }
    auto next = std::make_shared<gpu::FenceCycle>();
    {
        std::scoped_lock lock{*b};
        b->MarkGpuDirtyImpl();
        b->UpdateCycle(next);
    }
    Check(b->gpuWriteCycle == next, "new producer after full poll reused a prior producer");
    std::puts("PASS AllWrites and full poll stay conservative; new writer after cleanup gets a new producer");
}
static void InlineGpuDirtyWriteBecomesProducer() {
    Fixture f;
    auto b = f.buffer;
    auto p = std::make_shared<gpu::FenceCycle>();
    auto r = std::make_shared<gpu::FenceCycle>();
    p->complete = [&] { std::memset(b->backing->data(), 0x64, 4096); };
    AttachProducerAndReader(f, p, r);
    auto next = std::make_shared<gpu::FenceCycle>();
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    next->complete = [&] {
        entered.set_value();
        gate.wait();
        b->backing->data()[20] = 0x88;
    };
    unsigned copies{};
    u8 byte{0x88};
    {
        std::scoped_lock lock{*b};
        b->WriteImplStaged(span<u8>{&byte, 1}, 20, [&] {
            ++copies;
            b->BlockAllCpuBackingWrites();
        });
        b->UpdateCycle(next);
    }
    Check(copies && b->gpuWriteCycle == next, "inline/CBUF write on an already dirty buffer did not replace producer");
    auto store = std::async(std::launch::async, [&] { f.Write(37, 0x9a); });
    entered.get_future().wait();
    const bool retained = store.wait_for(100ms) == std::future_status::timeout;
    release.set_value();
    store.get();
    for (size_t i{}; i < 4096; ++i)
        Check(f.mirror[i] == (i == 37 ? 0x9a : i == 20 ? 0x88 : 0x64), "inline producer bytes were skipped");
    Check(retained, "inline producer dependency did not block");
    std::puts("PASS GPU-dirty inline/CBUF update replaces producer without a new dirty transition");
}
static void NestedUnlockCannotForgetRecordedWriter() {
    Fixture f;
    auto b = f.buffer;
    auto p = std::make_shared<gpu::FenceCycle>();
    auto r = std::make_shared<gpu::FenceCycle>();
    AttachProducerAndReader(f, p, r);
    auto next = std::make_shared<gpu::FenceCycle>();
    {
        std::scoped_lock outer{*b};
        b->MarkGpuDirtyImpl();
        { std::scoped_lock nested{*b}; }
        Check(!b->currentExecutionGpuDirty && b->pendingGpuWrite, "nested unlock discarded pending producer tracking");
        {
            std::scoped_lock stateLock{b->stateMutex};
            Check(b->GetGuestReadCycle() == r, "unsubmitted writer used an older producer");
        }
        b->UpdateCycle(next);
    }
    Check(b->gpuWriteCycle == next && !b->pendingGpuWrite, "recorded writer lost after nested unlock");
    std::puts("PASS pending writer survives nested resource unlock independently of barrier state");
}
