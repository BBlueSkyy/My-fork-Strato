#include <common/circular_queue.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using skyline::CircularQueue;

struct alignas(64) Frame {
    static inline std::atomic<int> live{0};
    int value;
    std::shared_ptr<int> reference;
    std::function<int()> callback;

    explicit Frame(int number)
        : value(number), reference(std::make_shared<int>(number)),
          callback([number] { return number; }) { ++live; }
    Frame(const Frame &other)
        : value(other.value), reference(other.reference), callback(other.callback) { ++live; }
    Frame(Frame &&other) noexcept
        : value(other.value), reference(std::move(other.reference)),
          callback(std::move(other.callback)) { ++live; }
    Frame &operator=(const Frame &) = delete;
    Frame &operator=(Frame &&) = delete;
    ~Frame() { --live; }
};

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    constexpr int Frames = 5000;
    std::atomic<int> received{0};
    std::atomic<long long> total{0};
    {
        CircularQueue<Frame> queue(5);
        std::thread consumer{[&] {
            queue.Process([&](const Frame &item) {
                require(reinterpret_cast<std::uintptr_t>(&item) % alignof(Frame) == 0, "Unaligned frame");
                require(item.reference && *item.reference == item.value, "Destroyed shared_ptr");
                require(item.callback && item.callback() == item.value, "Destroyed callback");
                total += item.value;
                ++received;
            }, [] {});
        }};
        for (int i{1}; i <= Frames; ++i)
            queue.Emplace(i);
        queue.Close();
        consumer.join();
        require(received == Frames, "Not all items were delivered");
        require(total == (static_cast<long long>(Frames) * (Frames + 1)) / 2, "Frame order/count incorrect");
    }
    require(Frame::live == 0, "Consumed frames leaked");

    {
        CircularQueue<Frame> queue(2);
        queue.Emplace(42);
        auto item{queue.Pop()};
        require(item.value == 42 && queue.Empty(), "Pop failed");
    }
    require(Frame::live == 0, "Popped frame leaked");

    {
        CircularQueue<Frame> source(2);
        source.Emplace(17);
        CircularQueue<Frame> moved(std::move(source));
        moved.Close();
        moved.Process([](const Frame &frame) { require(frame.value == 17, "Move lost a frame"); }, [] {});
    }
    require(Frame::live == 0, "Moved queue leaked");

    {
        CircularQueue<Frame> queue(2);
        queue.Emplace(99);
        // Destructor must clean up never-consumed elements too.
    }
    require(Frame::live == 0, "Unconsumed frame leaked");

    {
        CircularQueue<Frame> queue(1);
        std::thread consumer{[&] { queue.Process([](const Frame &) {}, [] {}); }};
        queue.Close();
        consumer.join();
    }

    std::cout << "PASS: aligned object lifetime, producer/consumer, close, pop, move and teardown\n";
}
