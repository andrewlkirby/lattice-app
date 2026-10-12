#pragma once

// Keeps the optimiser from removing a value or allocation that a test relies on.
namespace lattice::test
{
inline const void* volatile escapeSink = nullptr;

template <typename T>
void escape(T* p)
{
    escapeSink = p;
}
} // namespace lattice::test
