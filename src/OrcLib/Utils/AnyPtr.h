//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2021 ANSSI. All Rights Reserved.
//
// Author(s): fabienfl (ANSSI)
//

#pragma once

#include <memory>

#include <boost/smart_ptr/local_shared_ptr.hpp>

//
// AnyPtr is an abstraction layer providing the same generic pointer interface to stack or heap allocated resource.
//
// A template with an 'AnyType' attribute will grant the instanciator the choice of how the resource will be held:
// stack, reference or any smart pointer. The templated code will always refer to the allocated resource using
// a generic pointer interface like '->Foo()' instead of '.Foo()' for a stack allocation.
//
// A use case could be a wrapper which could own or reference the wrapped resource depending on the instanciator choice.
//
//
//
// template <typename BufferT>
// class BufferStreamWrapper
// {
//    BufferStreamWrapper(BufferT buffer)
//      : m_buffer(std::move(buffer)))
//    {
//    }
//
//    // The attribute 'm_buffer' can be allocated on the stack or be a reference
//    void PrintBufferAddress() {
//      std::cout << "Buffer address: " << m_buffer->data() << std::endl;
//    }
//
// private:
//   Any::Ptr<BufferT> m_buffer;
// }
//
// void FooInstanciator() {
//   // Keep ownership: using copy
//   {
//     std::vector<uint8_t> buffer;
//     BufferStreamWrapper wrapper(std::move(buffer));
//   }
//
//   // Give ownership: using move
//   {
//     std::vector<uint8_t> buffer;
//     BufferStreamWrapper wrapper(std::move(buffer));
//   }
//
//   // Keep ownership: using reference
//   {
//     std::vector<uint8_t> buffer;
//     BufferStreamWrapper wrapper(std::reference_wrapper(buffer));
//   }
//
//   // Give ownership: using move on unique_ptr
//   {
//      auto buffer = std::make_unique<std::vector<uint8_t>>();
//      BufferStreamWrapper wrapper(std::move(buffer));
//   }
//
//   // Share ownership: using a shared_ptr
//   {
//      auto buffer = std::make_shared<std::vector<uint8_t>>();
//      BufferStreamWrapper wrapper(buffer);
//   }
//
//   ...
// }
//

namespace Orc {

namespace details {

template <typename T>
class Stack
{
public:
    class PtrAdapter final
    {
    public:
        template <typename... Args>
        PtrAdapter(Args&&... args)
            : value(std::forward<Args>(args)...)
        {
        }

        constexpr T* operator->() { return &value; }
        constexpr T* operator->() const { return &value; }

        constexpr T& operator*() { return value; }
        constexpr const T& operator*() const { return value; }

    private:
        T value;
    };

    template <typename... Args>
    static constexpr PtrAdapter Make(Args&&... args)
    {
        return PtrAdapter(std::forward<Args>(args)...);
    }
};

template <typename T>
class Reference
{
public:
    class PtrAdapter final
    {
    public:
        PtrAdapter(T& ref)
            : value(ref)
        {
        }

        constexpr T* operator->() { return &value; }
        constexpr const T* operator->() const { return &value; }

        constexpr T& operator*() { return value; }
        constexpr const T& operator*() const { return value; }

    private:
        T& value;
    };

    static PtrAdapter Make(T& ref) { return PtrAdapter(ref); }
};

template <typename T>
struct AnyPtr
{
    using Type = typename Stack<T>::PtrAdapter;
};

template <typename T>
struct AnyPtr<std::reference_wrapper<T>>
{
    using Type = typename Reference<T>::PtrAdapter;
};

template <typename T>
struct AnyPtr<std::shared_ptr<T>>
{
    using Type = std::shared_ptr<T>;

    template <typename... Args>
    static Type Make(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

template <typename T>
struct AnyPtr<std::unique_ptr<T>>
{
    using Type = std::unique_ptr<T>;

    template <typename... Args>
    static Type Make(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }
};

template <typename T>
struct AnyPtr<boost::local_shared_ptr<T>>
{
    using Type = boost::local_shared_ptr<T>;

    template <typename... Args>
    static Type Make(Args&&... args)
    {
        return boost::make_local_shared<T>(std::forward<Args>(args)...);
    }
};

}  // namespace details

template <typename T>
using AnyPtr = typename details::AnyPtr<T>::Type;

}  // namespace Orc
