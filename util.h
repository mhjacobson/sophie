//
//  util.h
//  sophie
//
//  Created by Matt Jacobson on 5/26/22.
//

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <fstream>
#include <functional>
#include <string>

#ifndef UTIL_H
#define UTIL_H

struct DeleteImplicit {
    DeleteImplicit() = default;
    DeleteImplicit(DeleteImplicit &) = delete;
    DeleteImplicit(DeleteImplicit &&) = delete;
    DeleteImplicit &operator=(DeleteImplicit &) = delete;
    DeleteImplicit &operator=(DeleteImplicit &&) = delete;
    ~DeleteImplicit() = default;
};

template <typename T, size_t size>
struct RingBuffer : DeleteImplicit {
    RingBuffer(std::function<void(T&)> drop = [](T&){}) : _tail(0), _head(0), _drop(drop), _empty(true) { }

    void append(const T value) {
        if (!_empty && _tail == _head) {
            _drop(_values[_tail]);
            _tail = (_tail == size - 1) ? 0 : (_tail + 1);
        }

        _empty = false;
        _values[_head] = value;
        _head = (_head == size - 1) ? 0 : (_head + 1);
    }

    size_t count() const {
        if (_empty) {
            return 0;
        } else if (_head > _tail) {
            return (_head - _tail);
        } else {
            return (size - _tail) + _head;
        }
    }

    const T &operator[](const size_t index) const {
        assert(index < count());

        if (_tail + index >= size) {
            return _values[_tail + index - size];
        } else {
            return _values[_tail + index];
        }
    }

    bool is_empty() const {
        return _empty;
    }

    const T &last() const {
        assert(!_empty);
        return (*this)[count() - 1];
    }

    size_t count_where(const std::function<bool(const T&)> predicate) const {
        size_t count = 0;

        for (const T &value : *this) {
            if (predicate(value)) {
                count++;
            }
        }

        return count;
    }

    struct Iterator {
        Iterator(const RingBuffer<T, size> *const ring, size_t index) : _ring(ring), _index(index) {}

        Iterator &operator++() {
            _index++;
            return *this;
        }

        bool operator!=(const Iterator other) const {
            return _index != other._index;
        }

        const T &operator*() const {
            return (*_ring)[_index];
        }
    private:
        const RingBuffer<T, size> *_ring;
        size_t _index;
    };

    Iterator begin() const {
        return Iterator(this, 0);
    }

    Iterator end() const {
        return Iterator(this, count());
    }

private:
    T _values[size];
    size_t _tail;
    size_t _head;
    std::function<void(T&)> _drop;
    bool _empty;
};

template <unsigned int bucket_size>
struct Histogram : DeleteImplicit {
    Histogram() : _buckets{0} {}
    Histogram(const Histogram &other) {
        for (int i = 0; i < 256; i++) {
            _buckets[i] = other._buckets[i];
        }
    }

    void increment(const uint8_t value) {
        _buckets[value / bucket_size]++;
    }

    std::string description() const {
        std::string description = "";

        for (int i = 0; i < 256; i++) {
            if (_buckets[i] > 0) {
                description += std::to_string(i * bucket_size) + " - " + std::to_string((i + 1) * bucket_size - 1) + ": " + std::to_string(_buckets[i]) + ", ";
            }
        }

        return description;
    }

    uint32_t count_where(const std::function<bool(uint8_t)> predicate) const {
        uint32_t count = 0;

        for (int i = 0; i < 256; i++) {
            if (predicate(i * bucket_size)) {
                count += _buckets[i];
            }
        }

        return count;
    }
private:
    uint32_t _buckets[256];
};

[[maybe_unused]] static void move_file(const std::string source, const std::string destination) {
    const int rv = rename(source.c_str(), destination.c_str());

    if (rv != 0) {
        std::ifstream input(source, std::ios::binary);
        std::ofstream output(destination, std::ios::binary);
        char buf[1024];

        while (!input.eof()) {
            input.read(buf, sizeof (buf));
            assert(input.good() || input.eof());
            const std::streamsize count = input.gcount();
            output.write(buf, count);
            assert(output.good());
        }

        output.flush();
        remove(source.c_str());
    }
}

#endif /* UTIL_H */
