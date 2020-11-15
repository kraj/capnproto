// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// This file contains extended inline implementation details that are required along with async.h.
// We move this all into a separate file to make async.h more readable.
//
// Non-inline declarations here are defined in async.c++.

#pragma once

#ifndef KJ_ASYNC_H_INCLUDED
#error "Do not include this directly; include kj/async.h."
#include "async.h"  // help IDE parse this file
#endif

KJ_BEGIN_HEADER

namespace kj {
namespace _ {  // private

template <typename T>
class ExceptionOr;

class ExceptionOrValue {
public:
  ExceptionOrValue(bool, Exception&& exception): exception(kj::mv(exception)) {}
  KJ_DISALLOW_COPY(ExceptionOrValue);

  void addException(Exception&& exception) {
    if (this->exception == nullptr) {
      this->exception = kj::mv(exception);
    }
  }

  template <typename T>
  ExceptionOr<T>& as() { return *static_cast<ExceptionOr<T>*>(this); }
  template <typename T>
  const ExceptionOr<T>& as() const { return *static_cast<const ExceptionOr<T>*>(this); }

  Maybe<Exception> exception;

protected:
  // Allow subclasses to have move constructor / assignment.
  ExceptionOrValue() = default;
  ExceptionOrValue(ExceptionOrValue&& other) = default;
  ExceptionOrValue& operator=(ExceptionOrValue&& other) = default;
};

template <typename T>
class ExceptionOr: public ExceptionOrValue {
public:
  ExceptionOr() = default;
  ExceptionOr(T&& value): value(kj::mv(value)) {}
  ExceptionOr(bool, Exception&& exception): ExceptionOrValue(false, kj::mv(exception)) {}
  ExceptionOr(ExceptionOr&&) = default;
  ExceptionOr& operator=(ExceptionOr&&) = default;

  Maybe<T> value;
};

template <typename T>
inline T convertToReturn(ExceptionOr<T>&& result) {
  KJ_IF_MAYBE(value, result.value) {
    KJ_IF_MAYBE(exception, result.exception) {
      throwRecoverableException(kj::mv(*exception));
    }
    return _::returnMaybeVoid(kj::mv(*value));
  } else KJ_IF_MAYBE(exception, result.exception) {
    throwFatalException(kj::mv(*exception));
  } else {
    // Result contained neither a value nor an exception?
    KJ_UNREACHABLE;
  }
}

inline void convertToReturn(ExceptionOr<Void>&& result) {
  // Override <void> case to use throwRecoverableException().

  if (result.value != nullptr) {
    KJ_IF_MAYBE(exception, result.exception) {
      throwRecoverableException(kj::mv(*exception));
    }
  } else KJ_IF_MAYBE(exception, result.exception) {
    throwRecoverableException(kj::mv(*exception));
  } else {
    // Result contained neither a value nor an exception?
    KJ_UNREACHABLE;
  }
}

class Event {
  // An event waiting to be executed.  Not for direct use by applications -- promises use this
  // internally.

public:
  Event();
  Event(kj::EventLoop& loop);
  ~Event() noexcept(false);
  KJ_DISALLOW_COPY(Event);

  void armDepthFirst();
  // Enqueue this event so that `fire()` will be called from the event loop soon.
  //
  // Events scheduled in this way are executed in depth-first order:  if an event callback arms
  // more events, those events are placed at the front of the queue (in the order in which they
  // were armed), so that they run immediately after the first event's callback returns.
  //
  // Depth-first event scheduling is appropriate for events that represent simple continuations
  // of a previous event that should be globbed together for performance.  Depth-first scheduling
  // can lead to starvation, so any long-running task must occasionally yield with
  // `armBreadthFirst()`.  (Promise::then() uses depth-first whereas evalLater() uses
  // breadth-first.)
  //
  // To use breadth-first scheduling instead, use `armBreadthFirst()`.

  void armBreadthFirst();
  // Like `armDepthFirst()` except that the event is placed at the end of the queue.

  void armLast();
  // Enqueues this event to happen after all other events have run to completion and there is
  // really nothing left to do except wait for I/O.

  void disarm();
  // If the event is armed but hasn't fired, cancel it. (Destroying the event does this
  // implicitly.)

  kj::String trace();
  // Dump debug info about this event.

  virtual _::PromiseNode* getInnerForTrace();
  // If this event wraps a PromiseNode, get that node.  Used for debug tracing.
  // Default implementation returns nullptr.

protected:
  virtual Maybe<Own<Event>> fire() = 0;
  // Fire the event.  Possibly returns a pointer to itself, which will be discarded by the
  // caller.  This is the only way that an event can delete itself as a result of firing, as
  // doing so from within fire() will throw an exception.

private:
  friend class kj::EventLoop;
  EventLoop& loop;
  Event* next;
  Event** prev;
  bool firing = false;
};

class PromiseNode {
  // A Promise<T> contains a chain of PromiseNodes tracking the pending transformations.
  //
  // To reduce generated code bloat, PromiseNode is not a template.  Instead, it makes very hacky
  // use of pointers to ExceptionOrValue which actually point to ExceptionOr<T>, but are only
  // so down-cast in the few places that really need to be templated.  Luckily this is all
  // internal implementation details.

public:
  virtual void onReady(Event* event) noexcept = 0;
  // Arms the given event when ready.
  //
  // May be called multiple times. If called again before the event was armed, the old event will
  // never be armed, only the new one. If called again after the event was armed, the new event
  // will be armed immediately. Can be called with nullptr to un-register the existing event.

  virtual void setSelfPointer(Own<PromiseNode>* selfPtr) noexcept;
  // Tells the node that `selfPtr` is the pointer that owns this node, and will continue to own
  // this node until it is destroyed or setSelfPointer() is called again.  ChainPromiseNode uses
  // this to shorten redundant chains.  The default implementation does nothing; only
  // ChainPromiseNode should implement this.

  virtual void get(ExceptionOrValue& output) noexcept = 0;
  // Get the result.  `output` points to an ExceptionOr<T> into which the result will be written.
  // Can only be called once, and only after the node is ready.  Must be called directly from the
  // event loop, with no application code on the stack.

  virtual PromiseNode* getInnerForTrace();
  // If this node wraps some other PromiseNode, get the wrapped node.  Used for debug tracing.
  // Default implementation returns nullptr.

  template <typename T>
  static Own<PromiseNode> from(T&& promise) {
    // Given a Promise, extract the PromiseNode.
    return kj::mv(promise.node);
  }
  template <typename T>
  static PromiseNode& from(T& promise) {
    // Given a Promise, extract the PromiseNode.
    return *promise.node;
  }
  template <typename T>
  static T to(Own<PromiseNode>&& node) {
    // Construct a Promise from a PromiseNode. (T should be a Promise type.)
    return T(false, kj::mv(node));
  }

protected:
  class OnReadyEvent {
    // Helper class for implementing onReady().

  public:
    void init(Event* newEvent);

    void arm();
    void armBreadthFirst();
    // Arms the event if init() has already been called and makes future calls to init()
    // automatically arm the event.

  private:
    Event* event = nullptr;
  };
};

// -------------------------------------------------------------------

template <typename T>
inline NeverDone::operator Promise<T>() const {
  return PromiseNode::to<Promise<T>>(neverDone());
}

// -------------------------------------------------------------------

class ImmediatePromiseNodeBase: public PromiseNode {
public:
  ImmediatePromiseNodeBase();
  ~ImmediatePromiseNodeBase() noexcept(false);

  void onReady(Event* event) noexcept override;
};

template <typename T>
class ImmediatePromiseNode final: public ImmediatePromiseNodeBase {
  // A promise that has already been resolved to an immediate value or exception.

public:
  ImmediatePromiseNode(ExceptionOr<T>&& result): result(kj::mv(result)) {}

  void get(ExceptionOrValue& output) noexcept override {
    output.as<T>() = kj::mv(result);
  }

private:
  ExceptionOr<T> result;
};

class ImmediateBrokenPromiseNode final: public ImmediatePromiseNodeBase {
public:
  ImmediateBrokenPromiseNode(Exception&& exception);

  void get(ExceptionOrValue& output) noexcept override;

private:
  Exception exception;
};

// -------------------------------------------------------------------

class AttachmentPromiseNodeBase: public PromiseNode {
public:
  AttachmentPromiseNodeBase(Own<PromiseNode>&& dependency);

  void onReady(Event* event) noexcept override;
  void get(ExceptionOrValue& output) noexcept override;
  PromiseNode* getInnerForTrace() override;

private:
  Own<PromiseNode> dependency;

  void dropDependency();

  template <typename>
  friend class AttachmentPromiseNode;
};

template <typename Attachment>
class AttachmentPromiseNode final: public AttachmentPromiseNodeBase {
  // A PromiseNode that holds on to some object (usually, an Own<T>, but could be any movable
  // object) until the promise resolves.

public:
  AttachmentPromiseNode(Own<PromiseNode>&& dependency, Attachment&& attachment)
      : AttachmentPromiseNodeBase(kj::mv(dependency)),
        attachment(kj::mv<Attachment>(attachment)) {}

  ~AttachmentPromiseNode() noexcept(false) {
    // We need to make sure the dependency is deleted before we delete the attachment because the
    // dependency may be using the attachment.
    dropDependency();
  }

private:
  Attachment attachment;
};

// -------------------------------------------------------------------

#if __GNUC__ >= 8 && !__clang__
// GCC 8's class-memaccess warning rightly does not like the memcpy()'s below, but there's no
// "legal" way for us to extract the contetn of a PTMF so too bad.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif

class PtmfHelper {
  // This class is a private helper for GetFunctorStartAddress. The class represents the internal
  // representation of a pointer-to-member-function.

  template <typename... ParamTypes>
  friend struct GetFunctorStartAddress;

#if __GNUG__

  void* ptr;
  ptrdiff_t adj;
  // Layout of a pointer-to-member-function used by GCC and compatible compilers.

  void* apply(void* obj) {
#if defined(__arm__) || defined(__mips__) || defined(__aarch64__)
    if (adj & 1) {
      ptrdiff_t voff = (ptrdiff_t)ptr;
#else
    ptrdiff_t voff = (ptrdiff_t)ptr;
    if (voff & 1) {
      voff &= ~1;
#endif
      return *(void**)(*(char**)obj + voff);
    } else {
      return ptr;
    }
  }

#define BODY \
    PtmfHelper result; \
    static_assert(sizeof(p) == sizeof(result), "unknown ptmf layout"); \
    memcpy(&result, &p, sizeof(result)); \
    return result

#else  // __GNUG__

  void* apply(void* obj) { return nullptr; }
  // TODO(port):  PTMF instruction address extraction

#define BODY return PtmfHelper{}

#endif  // __GNUG__, else

  template <typename R, typename C, typename... P, typename F>
  static PtmfHelper from(F p) { BODY; }
  // Create a PtmfHelper from some arbitrary pointer-to-member-function which is not
  // overloaded nor a template. In this case the compiler is able to deduce the full function
  // signature directly given the name since there is only one function with that name.

  template <typename R, typename C, typename... P>
  static PtmfHelper from(R (C::*p)(NoInfer<P>...)) { BODY; }
  template <typename R, typename C, typename... P>
  static PtmfHelper from(R (C::*p)(NoInfer<P>...) const) { BODY; }
  // Create a PtmfHelper from some poniter-to-member-function which is a template. In this case
  // the function must match exactly the containing type C, return type R, and parameter types P...
  // GetFunctorStartAddress normally specifies exactly the correct C and R, but can only make a
  // guess at P. Luckily, if the function parameters are template parameters then it's not
  // necessary to be precise about P.
#undef BODY
};

#if __GNUC__ >= 8 && !__clang__
#pragma GCC diagnostic pop
#endif

template <typename... ParamTypes>
struct GetFunctorStartAddress {
  // Given a functor (any object defining operator()), return the start address of the function,
  // suitable for passing to addr2line to obtain a source file/line for debugging purposes.
  //
  // This turns out to be incredibly hard to implement in the presence of overloaded or templated
  // functors. Therefore, we impose these specific restrictions, specific to our use case:
  // - Overloading is not allowed, but templating is. (Generally we only intend to support lambdas
  //   anyway.)
  // - The template parameters to GetFunctorStartAddress specify a hint as to the expected
  //   parameter types. If the functor is templated, its parameters must match exactly these types.
  //   (If it's not templated, ParamTypes are ignored.)

  template <typename Func>
  static void* apply(Func&& func) {
    typedef decltype(func(instance<ParamTypes>()...)) ReturnType;
    return PtmfHelper::from<ReturnType, Decay<Func>, ParamTypes...>(
        &Decay<Func>::operator()).apply(&func);
  }
};

template <>
struct GetFunctorStartAddress<Void&&>: public GetFunctorStartAddress<> {};
// Hack for TransformPromiseNode use case: an input type of `Void` indicates that the function
// actually has no parameters.

class TransformPromiseNodeBase: public PromiseNode {
public:
  TransformPromiseNodeBase(Own<PromiseNode>&& dependency, void* continuationTracePtr);

  void onReady(Event* event) noexcept override;
  void get(ExceptionOrValue& output) noexcept override;
  PromiseNode* getInnerForTrace() override;

private:
  Own<PromiseNode> dependency;
  void* continuationTracePtr;

  void dropDependency();
  void getDepResult(ExceptionOrValue& output);

  virtual void getImpl(ExceptionOrValue& output) = 0;

  template <typename, typename, typename, typename>
  friend class TransformPromiseNode;
};

template <typename T, typename DepT, typename Func, typename ErrorFunc>
class TransformPromiseNode final: public TransformPromiseNodeBase {
  // A PromiseNode that transforms the result of another PromiseNode through an application-provided
  // function (implements `then()`).

public:
  TransformPromiseNode(Own<PromiseNode>&& dependency, Func&& func, ErrorFunc&& errorHandler)
      : TransformPromiseNodeBase(kj::mv(dependency),
            GetFunctorStartAddress<DepT&&>::apply(func)),
        func(kj::fwd<Func>(func)), errorHandler(kj::fwd<ErrorFunc>(errorHandler)) {}

  ~TransformPromiseNode() noexcept(false) {
    // We need to make sure the dependency is deleted before we delete the continuations because it
    // is a common pattern for the continuations to hold ownership of objects that might be in-use
    // by the dependency.
    dropDependency();
  }

private:
  Func func;
  ErrorFunc errorHandler;

  void getImpl(ExceptionOrValue& output) override {
    ExceptionOr<DepT> depResult;
    getDepResult(depResult);
    KJ_IF_MAYBE(depException, depResult.exception) {
      output.as<T>() = handle(
          MaybeVoidCaller<Exception, FixVoid<ReturnType<ErrorFunc, Exception>>>::apply(
              errorHandler, kj::mv(*depException)));
    } else KJ_IF_MAYBE(depValue, depResult.value) {
      output.as<T>() = handle(MaybeVoidCaller<DepT, T>::apply(func, kj::mv(*depValue)));
    }
  }

  ExceptionOr<T> handle(T&& value) {
    return kj::mv(value);
  }
  ExceptionOr<T> handle(PropagateException::Bottom&& value) {
    return ExceptionOr<T>(false, value.asException());
  }
};

// -------------------------------------------------------------------

class ForkHubBase;

class ForkBranchBase: public PromiseNode {
public:
  ForkBranchBase(Own<ForkHubBase>&& hub);
  ~ForkBranchBase() noexcept(false);

  void hubReady() noexcept;
  // Called by the hub to indicate that it is ready.

  // implements PromiseNode ------------------------------------------
  void onReady(Event* event) noexcept override;
  PromiseNode* getInnerForTrace() override;

protected:
  inline ExceptionOrValue& getHubResultRef();

  void releaseHub(ExceptionOrValue& output);
  // Release the hub.  If an exception is thrown, add it to `output`.

private:
  OnReadyEvent onReadyEvent;

  Own<ForkHubBase> hub;
  ForkBranchBase* next = nullptr;
  ForkBranchBase** prevPtr = nullptr;

  friend class ForkHubBase;
};

template <typename T> T copyOrAddRef(T& t) { return t; }
template <typename T> Own<T> copyOrAddRef(Own<T>& t) { return t->addRef(); }

template <typename T>
class ForkBranch final: public ForkBranchBase {
  // A PromiseNode that implements one branch of a fork -- i.e. one of the branches that receives
  // a const reference.

public:
  ForkBranch(Own<ForkHubBase>&& hub): ForkBranchBase(kj::mv(hub)) {}

  void get(ExceptionOrValue& output) noexcept override {
    ExceptionOr<T>& hubResult = getHubResultRef().template as<T>();
    KJ_IF_MAYBE(value, hubResult.value) {
      output.as<T>().value = copyOrAddRef(*value);
    } else {
      output.as<T>().value = nullptr;
    }
    output.exception = hubResult.exception;
    releaseHub(output);
  }
};

template <typename T, size_t index>
class SplitBranch final: public ForkBranchBase {
  // A PromiseNode that implements one branch of a fork -- i.e. one of the branches that receives
  // a const reference.

public:
  SplitBranch(Own<ForkHubBase>&& hub): ForkBranchBase(kj::mv(hub)) {}

  typedef kj::Decay<decltype(kj::get<index>(kj::instance<T>()))> Element;

  void get(ExceptionOrValue& output) noexcept override {
    ExceptionOr<T>& hubResult = getHubResultRef().template as<T>();
    KJ_IF_MAYBE(value, hubResult.value) {
      output.as<Element>().value = kj::mv(kj::get<index>(*value));
    } else {
      output.as<Element>().value = nullptr;
    }
    output.exception = hubResult.exception;
    releaseHub(output);
  }
};

// -------------------------------------------------------------------

class ForkHubBase: public Refcounted, protected Event {
public:
  ForkHubBase(Own<PromiseNode>&& inner, ExceptionOrValue& resultRef);

  inline ExceptionOrValue& getResultRef() { return resultRef; }

private:
  Own<PromiseNode> inner;
  ExceptionOrValue& resultRef;

  ForkBranchBase* headBranch = nullptr;
  ForkBranchBase** tailBranch = &headBranch;
  // Tail becomes null once the inner promise is ready and all branches have been notified.

  Maybe<Own<Event>> fire() override;
  _::PromiseNode* getInnerForTrace() override;

  friend class ForkBranchBase;
};

template <typename T>
class ForkHub final: public ForkHubBase {
  // A PromiseNode that implements the hub of a fork.  The first call to Promise::fork() replaces
  // the promise's outer node with a ForkHub, and subsequent calls add branches to that hub (if
  // possible).

public:
  ForkHub(Own<PromiseNode>&& inner): ForkHubBase(kj::mv(inner), result) {}

  Promise<_::UnfixVoid<T>> addBranch() {
    return _::PromiseNode::to<Promise<_::UnfixVoid<T>>>(kj::heap<ForkBranch<T>>(addRef(*this)));
  }

  _::SplitTuplePromise<T> split() {
    return splitImpl(MakeIndexes<tupleSize<T>()>());
  }

private:
  ExceptionOr<T> result;

  template <size_t... indexes>
  _::SplitTuplePromise<T> splitImpl(Indexes<indexes...>) {
    return kj::tuple(addSplit<indexes>()...);
  }

  template <size_t index>
  ReducePromises<typename SplitBranch<T, index>::Element> addSplit() {
    return _::PromiseNode::to<ReducePromises<typename SplitBranch<T, index>::Element>>(
        maybeChain(kj::heap<SplitBranch<T, index>>(addRef(*this)),
                   implicitCast<typename SplitBranch<T, index>::Element*>(nullptr)));
  }
};

inline ExceptionOrValue& ForkBranchBase::getHubResultRef() {
  return hub->getResultRef();
}

// -------------------------------------------------------------------

class ChainPromiseNode final: public PromiseNode, public Event {
  // Promise node which reduces Promise<Promise<T>> to Promise<T>.
  //
  // `Event` is only a public base class because otherwise we can't cast Own<ChainPromiseNode> to
  // Own<Event>.  Ugh, templates and private...

public:
  explicit ChainPromiseNode(Own<PromiseNode> inner);
  ~ChainPromiseNode() noexcept(false);

  void onReady(Event* event) noexcept override;
  void setSelfPointer(Own<PromiseNode>* selfPtr) noexcept override;
  void get(ExceptionOrValue& output) noexcept override;
  PromiseNode* getInnerForTrace() override;

private:
  enum State {
    STEP1,
    STEP2
  };

  State state;

  Own<PromiseNode> inner;
  // In STEP1, a PromiseNode for a Promise<T>.
  // In STEP2, a PromiseNode for a T.

  Event* onReadyEvent = nullptr;
  Own<PromiseNode>* selfPtr = nullptr;

  Maybe<Own<Event>> fire() override;
};

template <typename T>
Own<PromiseNode> maybeChain(Own<PromiseNode>&& node, Promise<T>*) {
  return heap<ChainPromiseNode>(kj::mv(node));
}

template <typename T>
Own<PromiseNode>&& maybeChain(Own<PromiseNode>&& node, T*) {
  return kj::mv(node);
}

template <typename T, typename Result = decltype(T::reducePromise(instance<Promise<T>>()))>
inline Result maybeReduce(Promise<T>&& promise, bool) {
  return T::reducePromise(kj::mv(promise));
}

template <typename T>
inline Promise<T> maybeReduce(Promise<T>&& promise, ...) {
  return kj::mv(promise);
}

// -------------------------------------------------------------------

class ExclusiveJoinPromiseNode final: public PromiseNode {
public:
  ExclusiveJoinPromiseNode(Own<PromiseNode> left, Own<PromiseNode> right);
  ~ExclusiveJoinPromiseNode() noexcept(false);

  void onReady(Event* event) noexcept override;
  void get(ExceptionOrValue& output) noexcept override;
  PromiseNode* getInnerForTrace() override;

private:
  class Branch: public Event {
  public:
    Branch(ExclusiveJoinPromiseNode& joinNode, Own<PromiseNode> dependency);
    ~Branch() noexcept(false);

    bool get(ExceptionOrValue& output);
    // Returns true if this is the side that finished.

    Maybe<Own<Event>> fire() override;
    _::PromiseNode* getInnerForTrace() override;

  private:
    ExclusiveJoinPromiseNode& joinNode;
    Own<PromiseNode> dependency;
  };

  Branch left;
  Branch right;
  OnReadyEvent onReadyEvent;
};

// -------------------------------------------------------------------

class ArrayJoinPromiseNodeBase: public PromiseNode {
public:
  ArrayJoinPromiseNodeBase(Array<Own<PromiseNode>> promises,
                           ExceptionOrValue* resultParts, size_t partSize);
  ~ArrayJoinPromiseNodeBase() noexcept(false);

  void onReady(Event* event) noexcept override final;
  void get(ExceptionOrValue& output) noexcept override final;
  PromiseNode* getInnerForTrace() override final;

protected:
  virtual void getNoError(ExceptionOrValue& output) noexcept = 0;
  // Called to compile the result only in the case where there were no errors.

private:
  uint countLeft;
  OnReadyEvent onReadyEvent;

  class Branch final: public Event {
  public:
    Branch(ArrayJoinPromiseNodeBase& joinNode, Own<PromiseNode> dependency,
           ExceptionOrValue& output);
    ~Branch() noexcept(false);

    Maybe<Own<Event>> fire() override;
    _::PromiseNode* getInnerForTrace() override;

    Maybe<Exception> getPart();
    // Calls dependency->get(output).  If there was an exception, return it.

  private:
    ArrayJoinPromiseNodeBase& joinNode;
    Own<PromiseNode> dependency;
    ExceptionOrValue& output;
  };

  Array<Branch> branches;
};

template <typename T>
class ArrayJoinPromiseNode final: public ArrayJoinPromiseNodeBase {
public:
  ArrayJoinPromiseNode(Array<Own<PromiseNode>> promises,
                       Array<ExceptionOr<T>> resultParts)
      : ArrayJoinPromiseNodeBase(kj::mv(promises), resultParts.begin(), sizeof(ExceptionOr<T>)),
        resultParts(kj::mv(resultParts)) {}

protected:
  void getNoError(ExceptionOrValue& output) noexcept override {
    auto builder = heapArrayBuilder<T>(resultParts.size());
    for (auto& part: resultParts) {
      KJ_IASSERT(part.value != nullptr,
                 "Bug in KJ promise framework:  Promise result had neither value no exception.");
      builder.add(kj::mv(*_::readMaybe(part.value)));
    }
    output.as<Array<T>>() = builder.finish();
  }

private:
  Array<ExceptionOr<T>> resultParts;
};

template <>
class ArrayJoinPromiseNode<void> final: public ArrayJoinPromiseNodeBase {
public:
  ArrayJoinPromiseNode(Array<Own<PromiseNode>> promises,
                       Array<ExceptionOr<_::Void>> resultParts);
  ~ArrayJoinPromiseNode();

protected:
  void getNoError(ExceptionOrValue& output) noexcept override;

private:
  Array<ExceptionOr<_::Void>> resultParts;
};

// -------------------------------------------------------------------

class EagerPromiseNodeBase: public PromiseNode, protected Event {
  // A PromiseNode that eagerly evaluates its dependency even if its dependent does not eagerly
  // evaluate it.

public:
  EagerPromiseNodeBase(Own<PromiseNode>&& dependency, ExceptionOrValue& resultRef);

  void onReady(Event* event) noexcept override;
  PromiseNode* getInnerForTrace() override;

private:
  Own<PromiseNode> dependency;
  OnReadyEvent onReadyEvent;

  ExceptionOrValue& resultRef;

  Maybe<Own<Event>> fire() override;
};

template <typename T>
class EagerPromiseNode final: public EagerPromiseNodeBase {
public:
  EagerPromiseNode(Own<PromiseNode>&& dependency)
      : EagerPromiseNodeBase(kj::mv(dependency), result) {}

  void get(ExceptionOrValue& output) noexcept override {
    output.as<T>() = kj::mv(result);
  }

private:
  ExceptionOr<T> result;
};

template <typename T>
Own<PromiseNode> spark(Own<PromiseNode>&& node) {
  // Forces evaluation of the given node to begin as soon as possible, even if no one is waiting
  // on it.
  return heap<EagerPromiseNode<T>>(kj::mv(node));
}

// -------------------------------------------------------------------

class AdapterPromiseNodeBase: public PromiseNode {
public:
  void onReady(Event* event) noexcept override;

protected:
  inline void setReady() {
    onReadyEvent.arm();
  }

private:
  OnReadyEvent onReadyEvent;
};

template <typename T, typename Adapter>
class AdapterPromiseNode final: public AdapterPromiseNodeBase,
                                private PromiseFulfiller<UnfixVoid<T>> {
  // A PromiseNode that wraps a PromiseAdapter.

public:
  template <typename... Params>
  AdapterPromiseNode(Params&&... params)
      : adapter(static_cast<PromiseFulfiller<UnfixVoid<T>>&>(*this), kj::fwd<Params>(params)...) {}

  void get(ExceptionOrValue& output) noexcept override {
    KJ_IREQUIRE(!isWaiting());
    output.as<T>() = kj::mv(result);
  }

private:
  ExceptionOr<T> result;
  bool waiting = true;
  Adapter adapter;

  void fulfill(T&& value) override {
    if (waiting) {
      waiting = false;
      result = ExceptionOr<T>(kj::mv(value));
      setReady();
    }
  }

  void reject(Exception&& exception) override {
    if (waiting) {
      waiting = false;
      result = ExceptionOr<T>(false, kj::mv(exception));
      setReady();
    }
  }

  bool isWaiting() override {
    return waiting;
  }
};

// -------------------------------------------------------------------

class FiberBase: public PromiseNode, private Event {
  // Base class for the outer PromiseNode representing a fiber.

public:
  explicit FiberBase(size_t stackSize, _::ExceptionOrValue& result);
  explicit FiberBase(const FiberPool& pool, _::ExceptionOrValue& result);
  ~FiberBase() noexcept(false);

  void start() { armDepthFirst(); }
  // Call immediately after construction to begin executing the fiber.

  class WaitDoneEvent;

  void onReady(_::Event* event) noexcept override;
  PromiseNode* getInnerForTrace() override;

protected:
  bool isFinished() { return state == FINISHED; }
  void destroy();

private:
  enum { WAITING, RUNNING, CANCELED, FINISHED } state;

  _::PromiseNode* currentInner = nullptr;
  OnReadyEvent onReadyEvent;
  Own<FiberStack> stack;
  _::ExceptionOrValue& result;

  void run();
  virtual void runImpl(WaitScope& waitScope) = 0;

  Maybe<Own<Event>> fire() override;
  // Implements Event. Each time the event is fired, switchToFiber() is called.

  friend class FiberStack;
  friend void _::waitImpl(Own<_::PromiseNode>&& node, _::ExceptionOrValue& result,
                          WaitScope& waitScope);
  friend bool _::pollImpl(_::PromiseNode& node, WaitScope& waitScope);
};

template <typename Func>
class Fiber final: public FiberBase {
public:
  explicit Fiber(size_t stackSize, Func&& func): FiberBase(stackSize, result), func(kj::fwd<Func>(func)) {}
  explicit Fiber(const FiberPool& pool, Func&& func): FiberBase(pool, result), func(kj::fwd<Func>(func)) {}
  ~Fiber() noexcept(false) { destroy(); }

  typedef FixVoid<decltype(kj::instance<Func&>()(kj::instance<WaitScope&>()))> ResultType;

  void get(ExceptionOrValue& output) noexcept override {
    KJ_IREQUIRE(isFinished());
    output.as<ResultType>() = kj::mv(result);
  }

private:
  Func func;
  ExceptionOr<ResultType> result;

  void runImpl(WaitScope& waitScope) override {
    result.template as<ResultType>() =
        MaybeVoidCaller<WaitScope&, ResultType>::apply(func, waitScope);
  }
};

}  // namespace _ (private)

// =======================================================================================

template <typename T>
Promise<T>::Promise(_::FixVoid<T> value)
    : PromiseBase(heap<_::ImmediatePromiseNode<_::FixVoid<T>>>(kj::mv(value))) {}

template <typename T>
Promise<T>::Promise(kj::Exception&& exception)
    : PromiseBase(heap<_::ImmediateBrokenPromiseNode>(kj::mv(exception))) {}

template <typename T>
template <typename Func, typename ErrorFunc>
PromiseForResult<Func, T> Promise<T>::then(Func&& func, ErrorFunc&& errorHandler) {
  typedef _::FixVoid<_::ReturnType<Func, T>> ResultT;

  Own<_::PromiseNode> intermediate =
      heap<_::TransformPromiseNode<ResultT, _::FixVoid<T>, Func, ErrorFunc>>(
          kj::mv(node), kj::fwd<Func>(func), kj::fwd<ErrorFunc>(errorHandler));
  auto result = _::PromiseNode::to<_::ChainPromises<_::ReturnType<Func, T>>>(
      _::maybeChain(kj::mv(intermediate), implicitCast<ResultT*>(nullptr)));
  return _::maybeReduce(kj::mv(result), false);
}

namespace _ {  // private

template <typename T>
struct IdentityFunc {
  inline T operator()(T&& value) const {
    return kj::mv(value);
  }
};
template <typename T>
struct IdentityFunc<Promise<T>> {
  inline Promise<T> operator()(T&& value) const {
    return kj::mv(value);
  }
};
template <>
struct IdentityFunc<void> {
  inline void operator()() const {}
};
template <>
struct IdentityFunc<Promise<void>> {
  Promise<void> operator()() const;
  // This can't be inline because it will make the translation unit depend on kj-async. Awkwardly,
  // Cap'n Proto relies on being able to include this header without creating such a link-time
  // dependency.
};

}  // namespace _ (private)

template <typename T>
template <typename ErrorFunc>
Promise<T> Promise<T>::catch_(ErrorFunc&& errorHandler) {
  // then()'s ErrorFunc can only return a Promise if Func also returns a Promise. In this case,
  // Func is being filled in automatically. We want to make sure ErrorFunc can return a Promise,
  // but we don't want the extra overhead of promise chaining if ErrorFunc doesn't actually
  // return a promise. So we make our Func return match ErrorFunc.
  return then(_::IdentityFunc<decltype(errorHandler(instance<Exception&&>()))>(),
              kj::fwd<ErrorFunc>(errorHandler));
}

template <typename T>
T Promise<T>::wait(WaitScope& waitScope) {
  _::ExceptionOr<_::FixVoid<T>> result;
  _::waitImpl(kj::mv(node), result, waitScope);
  return convertToReturn(kj::mv(result));
}

template <typename T>
bool Promise<T>::poll(WaitScope& waitScope) {
  return _::pollImpl(*node, waitScope);
}

template <typename T>
ForkedPromise<T> Promise<T>::fork() {
  return ForkedPromise<T>(false, refcounted<_::ForkHub<_::FixVoid<T>>>(kj::mv(node)));
}

template <typename T>
Promise<T> ForkedPromise<T>::addBranch() {
  return hub->addBranch();
}

template <typename T>
_::SplitTuplePromise<T> Promise<T>::split() {
  return refcounted<_::ForkHub<_::FixVoid<T>>>(kj::mv(node))->split();
}

template <typename T>
Promise<T> Promise<T>::exclusiveJoin(Promise<T>&& other) {
  return Promise(false, heap<_::ExclusiveJoinPromiseNode>(kj::mv(node), kj::mv(other.node)));
}

template <typename T>
template <typename... Attachments>
Promise<T> Promise<T>::attach(Attachments&&... attachments) {
  return Promise(false, kj::heap<_::AttachmentPromiseNode<Tuple<Attachments...>>>(
      kj::mv(node), kj::tuple(kj::fwd<Attachments>(attachments)...)));
}

template <typename T>
template <typename ErrorFunc>
Promise<T> Promise<T>::eagerlyEvaluate(ErrorFunc&& errorHandler) {
  // See catch_() for commentary.
  return Promise(false, _::spark<_::FixVoid<T>>(then(
      _::IdentityFunc<decltype(errorHandler(instance<Exception&&>()))>(),
      kj::fwd<ErrorFunc>(errorHandler)).node));
}

template <typename T>
Promise<T> Promise<T>::eagerlyEvaluate(decltype(nullptr)) {
  return Promise(false, _::spark<_::FixVoid<T>>(kj::mv(node)));
}

template <typename T>
kj::String Promise<T>::trace() {
  return PromiseBase::trace();
}

template <typename Func>
inline PromiseForResult<Func, void> evalLater(Func&& func) {
  return _::yield().then(kj::fwd<Func>(func), _::PropagateException());
}

template <typename Func>
inline PromiseForResult<Func, void> evalLast(Func&& func) {
  return _::yieldHarder().then(kj::fwd<Func>(func), _::PropagateException());
}

template <typename Func>
inline PromiseForResult<Func, void> evalNow(Func&& func) {
  PromiseForResult<Func, void> result = nullptr;
  KJ_IF_MAYBE(e, kj::runCatchingExceptions([&]() {
    result = func();
  })) {
    result = kj::mv(*e);
  }
  return result;
}

template <typename Func>
struct RetryOnDisconnect_ {
  static inline PromiseForResult<Func, void> apply(Func&& func) {
    return evalLater([func = kj::mv(func)]() mutable -> PromiseForResult<Func, void> {
      auto promise = evalNow(func);
      return promise.catch_([func = kj::mv(func)](kj::Exception&& e) mutable -> PromiseForResult<Func, void> {
        if (e.getType() == kj::Exception::Type::DISCONNECTED) {
          return func();
        } else {
          return kj::mv(e);
        }
      });
    });
  }
};
template <typename Func>
struct RetryOnDisconnect_<Func&> {
  // Specialization for references. Needed because the syntax for capturing references in a
  // lambda is different. :(
  static inline PromiseForResult<Func, void> apply(Func& func) {
    auto promise = evalLater(func);
    return promise.catch_([&func](kj::Exception&& e) -> PromiseForResult<Func, void> {
      if (e.getType() == kj::Exception::Type::DISCONNECTED) {
        return func();
      } else {
        return kj::mv(e);
      }
    });
  }
};

template <typename Func>
inline PromiseForResult<Func, void> retryOnDisconnect(Func&& func) {
  return RetryOnDisconnect_<Func>::apply(kj::fwd<Func>(func));
}

template <typename Func>
inline PromiseForResult<Func, WaitScope&> startFiber(size_t stackSize, Func&& func) {
  typedef _::FixVoid<_::ReturnType<Func, WaitScope&>> ResultT;

  Own<_::FiberBase> intermediate = kj::heap<_::Fiber<Func>>(stackSize, kj::fwd<Func>(func));
  intermediate->start();
  auto result = _::PromiseNode::to<_::ChainPromises<_::ReturnType<Func, WaitScope&>>>(
      _::maybeChain(kj::mv(intermediate), implicitCast<ResultT*>(nullptr)));
  return _::maybeReduce(kj::mv(result), false);
}

template <typename Func>
inline PromiseForResult<Func, WaitScope&> FiberPool::startFiber(Func&& func) const {
  typedef _::FixVoid<_::ReturnType<Func, WaitScope&>> ResultT;

  Own<_::FiberBase> intermediate = kj::heap<_::Fiber<Func>>(*this, kj::fwd<Func>(func));
  intermediate->start();
  auto result = _::PromiseNode::to<_::ChainPromises<_::ReturnType<Func, WaitScope&>>>(
      _::maybeChain(kj::mv(intermediate), implicitCast<ResultT*>(nullptr)));
  return _::maybeReduce(kj::mv(result), false);
}

template <typename T>
template <typename ErrorFunc>
void Promise<T>::detach(ErrorFunc&& errorHandler) {
  return _::detach(then([](T&&) {}, kj::fwd<ErrorFunc>(errorHandler)));
}

template <>
template <typename ErrorFunc>
void Promise<void>::detach(ErrorFunc&& errorHandler) {
  return _::detach(then([]() {}, kj::fwd<ErrorFunc>(errorHandler)));
}

template <typename T>
Promise<Array<T>> joinPromises(Array<Promise<T>>&& promises) {
  return _::PromiseNode::to<Promise<Array<T>>>(kj::heap<_::ArrayJoinPromiseNode<T>>(
      KJ_MAP(p, promises) { return _::PromiseNode::from(kj::mv(p)); },
      heapArray<_::ExceptionOr<T>>(promises.size())));
}

// =======================================================================================

namespace _ {  // private

template <typename T>
class WeakFulfiller final: public PromiseFulfiller<T>, private kj::Disposer {
  // A wrapper around PromiseFulfiller which can be detached.
  //
  // There are a couple non-trivialities here:
  // - If the WeakFulfiller is discarded, we want the promise it fulfills to be implicitly
  //   rejected.
  // - We cannot destroy the WeakFulfiller until the application has discarded it *and* it has been
  //   detached from the underlying fulfiller, because otherwise the later detach() call will go
  //   to a dangling pointer.  Essentially, WeakFulfiller is reference counted, although the
  //   refcount never goes over 2 and we manually implement the refcounting because we need to do
  //   other special things when each side detaches anyway.  To this end, WeakFulfiller is its own
  //   Disposer -- dispose() is called when the application discards its owned pointer to the
  //   fulfiller and detach() is called when the promise is destroyed.

public:
  KJ_DISALLOW_COPY(WeakFulfiller);

  static kj::Own<WeakFulfiller> make() {
    WeakFulfiller* ptr = new WeakFulfiller;
    return Own<WeakFulfiller>(ptr, *ptr);
  }

  void fulfill(FixVoid<T>&& value) override {
    if (inner != nullptr) {
      inner->fulfill(kj::mv(value));
    }
  }

  void reject(Exception&& exception) override {
    if (inner != nullptr) {
      inner->reject(kj::mv(exception));
    }
  }

  bool isWaiting() override {
    return inner != nullptr && inner->isWaiting();
  }

  void attach(PromiseFulfiller<T>& newInner) {
    inner = &newInner;
  }

  void detach(PromiseFulfiller<T>& from) {
    if (inner == nullptr) {
      // Already disposed.
      delete this;
    } else {
      KJ_IREQUIRE(inner == &from);
      inner = nullptr;
    }
  }

private:
  mutable PromiseFulfiller<T>* inner;

  WeakFulfiller(): inner(nullptr) {}

  void disposeImpl(void* pointer) const override {
    // TODO(perf): Factor some of this out so it isn't regenerated for every fulfiller type?

    if (inner == nullptr) {
      // Already detached.
      delete this;
    } else {
      if (inner->isWaiting()) {
        inner->reject(kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
            kj::heapString("PromiseFulfiller was destroyed without fulfilling the promise.")));
      }
      inner = nullptr;
    }
  }
};

template <typename T>
class PromiseAndFulfillerAdapter {
public:
  PromiseAndFulfillerAdapter(PromiseFulfiller<T>& fulfiller,
                             WeakFulfiller<T>& wrapper)
      : fulfiller(fulfiller), wrapper(wrapper) {
    wrapper.attach(fulfiller);
  }

  ~PromiseAndFulfillerAdapter() noexcept(false) {
    wrapper.detach(fulfiller);
  }

private:
  PromiseFulfiller<T>& fulfiller;
  WeakFulfiller<T>& wrapper;
};

}  // namespace _ (private)

template <typename T>
template <typename Func>
bool PromiseFulfiller<T>::rejectIfThrows(Func&& func) {
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions(kj::mv(func))) {
    reject(kj::mv(*exception));
    return false;
  } else {
    return true;
  }
}

template <typename Func>
bool PromiseFulfiller<void>::rejectIfThrows(Func&& func) {
  KJ_IF_MAYBE(exception, kj::runCatchingExceptions(kj::mv(func))) {
    reject(kj::mv(*exception));
    return false;
  } else {
    return true;
  }
}

template <typename T, typename Adapter, typename... Params>
_::ReducePromises<T> newAdaptedPromise(Params&&... adapterConstructorParams) {
  Own<_::PromiseNode> intermediate(
      heap<_::AdapterPromiseNode<_::FixVoid<T>, Adapter>>(
          kj::fwd<Params>(adapterConstructorParams)...));
  return _::PromiseNode::to<_::ReducePromises<T>>(
      _::maybeChain(kj::mv(intermediate), implicitCast<T*>(nullptr)));
}

template <typename T>
PromiseFulfillerPair<T> newPromiseAndFulfiller() {
  auto wrapper = _::WeakFulfiller<T>::make();

  Own<_::PromiseNode> intermediate(
      heap<_::AdapterPromiseNode<_::FixVoid<T>, _::PromiseAndFulfillerAdapter<T>>>(*wrapper));
  auto promise = _::PromiseNode::to<_::ReducePromises<T>>(
      _::maybeChain(kj::mv(intermediate), implicitCast<T*>(nullptr)));

  return PromiseFulfillerPair<T> { kj::mv(promise), kj::mv(wrapper) };
}

// =======================================================================================
// cross-thread stuff

namespace _ {  // (private)

class XThreadEvent: private Event,         // it's an event in the target thread
                    public PromiseNode {   // it's a PromiseNode in the requesting thread
public:
  XThreadEvent(ExceptionOrValue& result, const Executor& targetExecutor);

protected:
  void ensureDoneOrCanceled();
  // MUST be called in destructor of subclasses to make sure the object is not destroyed while
  // still being accessed by the other thread. (This can't be placed in ~XThreadEvent() because
  // that destructor doesn't run until the subclass has already been destroyed.)

  virtual kj::Maybe<Own<PromiseNode>> execute() = 0;
  // Run the function. If the function returns a promise, returns the inner PromiseNode, otherwise
  // returns null.

  // implements PromiseNode ----------------------------------------------------
  void onReady(Event* event) noexcept override;

private:
  ExceptionOrValue& result;

  kj::Own<const Executor> targetExecutor;
  Maybe<const Executor&> replyExecutor;  // If executeAsync() was used.

  kj::Maybe<Own<PromiseNode>> promiseNode;
  // Accessed only in target thread.

  Maybe<XThreadEvent&> targetNext;
  Maybe<XThreadEvent&>* targetPrev = nullptr;
  // Membership in one of the linked lists in the target Executor's work list or cancel list. These
  // fields are protected by the target Executor's mutex.

  enum {
    UNUSED,
    // Object was never queued on another thread.

    QUEUED,
    // Target thread has not yet dequeued the event from the state.start list. The requesting
    // thread can cancel execution by removing the event from the list.

    EXECUTING,
    // Target thread has dequeued the event from state.start and moved it to state.executing. To
    // cancel, the requesting thread must add the event to the state.cancel list and change the
    // state to CANCELING.

    CANCELING,
    // Requesting thread is trying to cancel this event. The target thread will change the state to
    // `DONE` once canceled.

    DONE
    // Target thread has completed handling this event and will not touch it again. The requesting
    // thread can safely delete the object. The `state` is updated to `DONE` using an atomic
    // release operation after ensuring that the event will not be touched again, so that the
    // requesting can safely skip locking if it observes the state is already DONE.
  } state = UNUSED;
  // State, which is also protected by `targetExecutor`'s mutex.

  Maybe<XThreadEvent&> replyNext;
  Maybe<XThreadEvent&>* replyPrev = nullptr;
  // Membership in `replyExecutor`'s reply list. Protected by `replyExecutor`'s mutex. The
  // executing thread places the event in the reply list near the end of the `EXECUTING` state.
  // Because the thread cannot lock two mutexes at once, it's possible that the reply executor
  // will receive the reply while the event is still listed in the EXECUTING state, but it can
  // ignore the state and proceed with the result.

  OnReadyEvent onReadyEvent;
  // Accessed only in requesting thread.

  friend class kj::Executor;

  void done();
  // Sets the state to `DONE` and notifies the originating thread that this event is done. Do NOT
  // call under lock.

  void sendReply();
  // Notifies the originating thread that this event is done, but doesn't set the state to DONE
  // yet. Do NOT call under lock.

  void setDoneState();
  // Assigns `state` to `DONE`, being careful to use an atomic-release-store if needed. This must
  // only be called in the destination thread, and must either be called under lock, or the thread
  // must take the lock and release it again shortly after setting the state (because some threads
  // may be waiting on the DONE state using a conditional wait on the mutex). After calling
  // setDoneState(), the destination thread MUST NOT touch this object ever again; it now belongs
  // solely to the requesting thread.

  void setDisconnected();
  // Sets the result to a DISCONNECTED exception indicating that the target event loop exited.

  class DelayedDoneHack;

  // implements Event ----------------------------------------------------------
  Maybe<Own<Event>> fire() override;
  // If called with promiseNode == nullptr, it's time to call execute(). If promiseNode != nullptr,
  // then it just indicated readiness and we need to get its result.
};

template <typename Func, typename = _::FixVoid<_::ReturnType<Func, void>>>
class XThreadEventImpl final: public XThreadEvent {
  // Implementation for a function that does not return a Promise.
public:
  XThreadEventImpl(Func&& func, const Executor& target)
      : XThreadEvent(result, target), func(kj::fwd<Func>(func)) {}
  ~XThreadEventImpl() noexcept(false) { ensureDoneOrCanceled(); }

  typedef _::FixVoid<_::ReturnType<Func, void>> ResultT;

  kj::Maybe<Own<_::PromiseNode>> execute() override {
    result.value = MaybeVoidCaller<Void, FixVoid<decltype(func())>>::apply(func, Void());
    return nullptr;
  }

  // implements PromiseNode ----------------------------------------------------
  void get(ExceptionOrValue& output) noexcept override {
    output.as<ResultT>() = kj::mv(result);
  }

private:
  Func func;
  ExceptionOr<ResultT> result;
  friend Executor;
};

template <typename Func, typename T>
class XThreadEventImpl<Func, Promise<T>> final: public XThreadEvent {
  // Implementation for a function that DOES return a Promise.
public:
  XThreadEventImpl(Func&& func, const Executor& target)
      : XThreadEvent(result, target), func(kj::fwd<Func>(func)) {}
  ~XThreadEventImpl() noexcept(false) { ensureDoneOrCanceled(); }

  typedef _::FixVoid<_::UnwrapPromise<PromiseForResult<Func, void>>> ResultT;

  kj::Maybe<Own<_::PromiseNode>> execute() override {
    auto result = _::PromiseNode::from(func());
    KJ_IREQUIRE(result.get() != nullptr);
    return kj::mv(result);
  }

  // implements PromiseNode ----------------------------------------------------
  void get(ExceptionOrValue& output) noexcept override {
    output.as<ResultT>() = kj::mv(result);
  }

private:
  Func func;
  ExceptionOr<ResultT> result;
  friend Executor;
};

}  // namespace _ (private)

template <typename Func>
_::UnwrapPromise<PromiseForResult<Func, void>> Executor::executeSync(Func&& func) const {
  _::XThreadEventImpl<Func> event(kj::fwd<Func>(func), *this);
  send(event, true);
  return convertToReturn(kj::mv(event.result));
}

template <typename Func>
PromiseForResult<Func, void> Executor::executeAsync(Func&& func) const {
  auto event = kj::heap<_::XThreadEventImpl<Func>>(kj::fwd<Func>(func), *this);
  send(*event, false);
  return _::PromiseNode::to<PromiseForResult<Func, void>>(kj::mv(event));
}

}  // namespace kj

#if KJ_HAVE_COROUTINE

// =======================================================================================
// Coroutines TS integration with kj::Promise<T>.
//
// Here's a simple coroutine:
//
//   Promise<Own<AsyncIoStream>> connectToService(Network& n) {
//     auto a = co_await n.parseAddress(IP, PORT);
//     auto c = co_await a->connect();
//     co_return kj::mv(c);
//   }
//
// The presence of the co_await and co_return keywords tell the compiler it is a coroutine.
// Although it looks similar to a function, it has a couple large differences. First, everything
// that would normally live in the stack frame lives instead in a heap-based coroutine frame.
// Second, the coroutine has the ability to return from its scope without deallocating this frame
// (to suspend, in other words), and the ability to resume from its last suspension point.
//
// In order to know how to suspend, resume, and return from a coroutine, the compiler looks up a
// coroutine implementation type via a traits class parameterized by the coroutine return and
// parameter types. We'll name our coroutine implementation `kj::_::Coroutine<T>`,

namespace kj::_ { template <typename T> class Coroutine; }

// Specializing the appropriate traits class tells the compiler about `kj::_::Coroutine<T>`.

namespace std::experimental {

template <class T, class... Args>
struct coroutine_traits<kj::Promise<T>, Args...> {
  // `Args...` are the coroutine's parameter types.

  using promise_type = kj::_::Coroutine<T>;
  // The Coroutines TS calls this the "promise type". This makes sense when thinking of coroutines
  // returning `std::future<T>`, since the coroutine implementation would be a wrapper around
  // a `std::promise<T>`. It's extremely confusing from a KJ perspective, however, so I call it
  // the "coroutine implementation type" instead.
};

}  // namespace std::experimental

// Now when the compiler sees our `connectToService()` coroutine above, it default-constructs a
// `coroutine_traits<Promise<Own<AsyncIoStream>>, Network&>::promise_type`, or
// `kj::_::Coroutine<Own<AsyncIoStream>>`.
//
// The implementation object lives in the heap-allocated coroutine frame. It gets destroyed and
// deallocated when the frame does.

namespace kj::_ {

template <typename Self, typename T>
class CoroutineBase;
// CRTP mixin, covered later.

template <typename T>
class Coroutine: public CoroutineBase<Coroutine<T>, T> {
  // The standard calls this the `promise_type` object. We can call this the "coroutine
  // implementation object" since the word promise means different things in KJ and std styles. This
  // is where we implement how a `kj::Promise<T>` is returned from a coroutine, and how that promise
  // is later fulfilled. We also fill in a few lifetime-related details.
  //
  // The implementation object is also where we can customize memory allocation of coroutine frames,
  // by implementing a member `operator new(size_t, Args...)` (same `Args...` as in
  // coroutine_traits).
  //
  // We can also customize how await-expressions are transformed within `kj::Promise<T>`-based
  // coroutines by implementing an `await_transform(P)` member function, where `P` is some type for
  // which we want to implement co_await support, e.g. `kj::Promise<U>`. This feature is useful
  // to provide a more optimized `kj::EventLoop` integration when the coroutine's return type and
  // the await-expression's type are both `kj::Promise` instantiations.

public:
  using Handle = std::experimental::coroutine_handle<Coroutine<T>>;

  ~Coroutine() { if (adapter) adapter->coroutine = nullptr; }
  // The implementation object (*this) lives inside the coroutine's frame, so if it is being
  // destroyed, then that means the frame is being destroyed. Since the frame might be destroyed
  // *before* the adapter (e.g., the coroutine resumes from its last suspension point before the
  // user destroys the owning promise), we need to tell the adapter not to try to double-free the
  // frame with coroutine.destroy().
  //
  // Also see the implementation of ~Adapter().

  Promise<T> get_return_object() {
    return newAdaptedPromise<T, Adapter>(Handle::from_promise(*this));
  }

  auto initial_suspend() { return std::experimental::suspend_never(); }
  auto final_suspend() noexcept { return std::experimental::suspend_never(); }
  // These adjust the suspension behavior of coroutines immediately upon initiation, and immediately
  // after completion.
  //
  // The initial suspension point might allow coroutines to always propagate their exceptions
  // asynchronously, even ones thrown before the first co_await.
  //
  // The final suspension point could be useful to delay deallocation of the coroutine frame to
  // match the lifetime of the enclosing promise.

  void unhandled_exception() {
    adapter->fulfiller.rejectIfThrows([] { std::rethrow_exception(std::current_exception()); });
  }

  template <typename U>
  class Awaiter;

  template <typename U>
  Awaiter<U> await_transform(kj::Promise<U>& promise) { return Awaiter<U>(kj::mv(promise)); }
  template <typename U>
  Awaiter<U> await_transform(kj::Promise<U>&& promise) { return Awaiter<U>(kj::mv(promise)); }
  // Called when someone writes `co_await promise`, where `promise` is a kj::Promise<U>. We return
  // an Awaiter<U>, which implements coroutine suspension and resumption in terms of the KJ async
  // event system.
  //
  // There is another hook we could implement: an `operator co_await()` free function. However, a
  // free function would be unaware of the type of the enclosing coroutine. Since Awaiter<U> is a
  // member class template of Coroutine<T>, it is able to implement an
  // `await_suspend(Coroutine<T>::Handle)` override, providing it type-safe access to our enclosing
  // coroutine's promise adapter. An `operator co_await()` free function would have to implement
  // a type-erased `await_suspend(std::experimental::coroutine_handle<void>)` override, and
  // implement suspension and resumption in terms of .then(). Yuck!

private:
  friend class CoroutineBase<Coroutine<T>, T>;

  struct Adapter;
  Adapter* adapter = nullptr;
  // Set by Adapter's ctor in `get_return_object()`. Nulled out again by Adapter's dtor, if the
  // promise returned from `get_return_object()` is destroyed while this coroutine is suspended.
};

template <typename Self, typename T>
class CoroutineBase {
public:
  template <typename U>
  void return_value(U&& value) {
    static_cast<Self*>(this)->adapter->fulfiller.fulfill(T(kj::fwd<U>(value)));
  }
};
template <typename Self>
class CoroutineBase<Self, void> {
public:
  void return_void() {
    static_cast<Self*>(this)->adapter->fulfiller.fulfill();
  }
};
// The Coroutines spec has no `_::FixVoid<T>` equivalent to unify valueful and valueless co_return
// statements, and programs are ill-formed if the coroutine implementation object (Coroutine<T>) has
// both a `return_value()` and `return_void()`. No amount of EnableIffery can get around it, so
// these return_* functions live in a CRTP mixin.

template <typename T>
struct Coroutine<T>::Adapter: public Event {
  explicit Adapter(PromiseFulfiller<T>& f, Coroutine::Handle coroutine)
      : fulfiller(f), coroutine(coroutine) {
    coroutine.promise().adapter = this;
  }
  KJ_DISALLOW_COPY(Adapter);

  ~Adapter() noexcept(false) {
    // If a coroutine runs to completion, its frame will already have been destroyed by the time the
    // adapter is destroyed, so this if-condition will almost always be false.
    if (coroutine) {
      // coroutine.promise() is a reference to our implementation object, Coroutine<T>.
      coroutine.promise().adapter = nullptr;
      coroutine.destroy();
    }
  }

  Maybe<Own<Event>> fire() override {
    // The promise this coroutine is currently waiting on is ready. Extract its result and check for
    // exceptions.

    node->get(*result);

    // Note: the Promise::wait() implementation throws a recoverable exception if both a value and
    // an exception are set. We only have visibility on the exception here, so this might not have
    // quite the same semantics.

    KJ_IF_MAYBE(exception, result->exception) {
      fulfiller.reject(kj::mv(*exception));
      // The coroutine threw an exception. Our only choice is to destroy it.
      //
      // TODO(now): Should we ignore exceptions from destroy()?
      coroutine.destroy();
    } else {
      // Call Awaiter::await_resume() and proceed with the coroutine. Note that if this was the last
      // co_await in the coroutine, control will flow off the end of the coroutine and it will be
      // destroyed -- as part of the execution of coroutine.resume().
      coroutine.resume();
    }

    return nullptr;
  }

  PromiseFulfiller<T>& fulfiller;
  // Used to fulfill/reject the enclosing coroutine's kj::Promise<T>.

  Coroutine::Handle coroutine;

  PromiseNode* node = nullptr;
  ExceptionOrValue* result = nullptr;
  // These pointers are both set by Awaiter::await_suspend().
};

template <typename T>
template <typename U>
class Coroutine<T>::Awaiter {
public:
  explicit Awaiter(Promise<U> promise): promise(kj::mv(promise)) {}

  bool await_ready() const { return false; }
  // This could return "`promise.node.get()` is safe to call" instead, which would make suspension-
  // less co_awaits possible for immediately-fulfilled promises. However, that may be unwise, since
  // it is explicitly not how .then() works.

  U await_resume() {
    auto value = kj::_::readMaybe(result.value);
    KJ_IASSERT(value != nullptr, "Coroutine::Adapter::fire() should have checked for exceptions "
                                 "before resuming the coroutine.");
    return kj::mv(*value);
  }

  void await_suspend(Coroutine::Handle coroutine) {
    auto& node = PromiseNode::from(promise);
    auto adapter = coroutine.promise().adapter;

    // Before suspending, schedule a wakeup call for when the promise we're waiting on is ready.
    // We use the enclosing coroutine's promise adapter as an Event. It's tempting to make this
    // Awaiter be the Event, since this class is where both the promise and space for the result
    // of the promise live, meaning it can most conveniently extract the result. However, whatever
    // Event is used must be able to destroy the coroutine from within the `fire()` callback,
    // which rules out using anything that lives inside the coroutine frame as the Event,
    // including Awaiter and Coroutine<T> itself. The Adapter is the only object that lives
    // outside the coroutine frame.
    node.onReady(adapter);

    // Give the adapter some pointers to our internals so that it can perform the result
    // extraction. It doesn't need to know anything about the type U, but it needs to be able to
    // check for exceptions, in which case the coroutine can be be immediately destroyed (the
    // alternative being a pointless resume only to rethrow the exception).
    adapter->node = &node;
    // Awaiter is movable, but it is safe to store a pointer to `result` in Adapter, because
    // by the time `await_suspend()` is called, Awaiter has already been returned from
    // `await_transform()` and stored in its final resting place in the coroutine's frame.
    adapter->result = &result;
  }

private:
  Promise<U> promise;
  ExceptionOr<FixVoid<U>> result;
};

}  // namespace kj::_ (private)

#endif  // KJ_HAVE_COROUTINE

KJ_END_HEADER
