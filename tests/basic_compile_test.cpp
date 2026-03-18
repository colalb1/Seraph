#include "seraph/queue.hpp"
#include "seraph/ringbuffer.hpp"
#include "seraph/stack.hpp"

#include <array>

int main() {
    seraph::stack<int> stack;
    stack.push(1);
    stack.push(2);

    if (stack.top() != 2) {
        return 1;
    }

    if (stack.pop() != 2) {
        return 1;
    }

    seraph::stack<int> adaptive_stack;
    adaptive_stack.push(10);
    adaptive_stack.emplace(20);

    if (adaptive_stack.top() != 20) {
        return 1;
    }

    if (adaptive_stack.pop() != 20) {
        return 1;
    }

    seraph::queue<int> queue;
    if (!queue.empty()) {
        return 1;
    }

    queue.push(1);
    queue.emplace(2);

    if (queue.front() != 1) {
        return 1;
    }

    if (queue.back() != 2) {
        return 1;
    }

    if (queue.size() != 2) {
        return 1;
    }

    if (queue.pop() != 1) {
        return 1;
    }

    if (queue.pop() != 2) {
        return 1;
    }

    if (!queue.empty()) {
        return 1;
    }

    if (queue.pop().has_value()) {
        return 1;
    }

    std::array<int, 4> values = {3, 4, 5, 6};
    queue.push_range(values.begin(), values.end());

    if (queue.size() != values.size()) {
        return 1;
    }

    for (int expected : values) {
        if (queue.front() != expected) {
            return 1;
        }

        if (queue.pop() != expected) {
            return 1;
        }
    }

    if (!queue.empty()) {
        return 1;
    }

    seraph::RingBuffer<int> ringbuffer(8);
    if (!ringbuffer.empty()) {
        return 1;
    }

    ringbuffer.push(11);
    ringbuffer.emplace(12);

    if (ringbuffer.front() != 11) {
        return 1;
    }

    if (ringbuffer.back() != 12) {
        return 1;
    }

    if (ringbuffer.size() != 2) {
        return 1;
    }

    if (ringbuffer.pop() != 11) {
        return 1;
    }

    if (ringbuffer.pop() != 12) {
        return 1;
    }

    if (!ringbuffer.empty()) {
        return 1;
    }

    if (ringbuffer.pop().has_value()) {
        return 1;
    }

    return 0;
}
