#include "isolate/class_handle.h"
#include "isolate/util.h"
#include "lib/lockable.h"
#include "reference_handle.h"
#include "transferable.h"
#include "transferable_handle.h"
#include "external_copy.h"
#include "external_copy_handle.h"
#include <deque>

using namespace v8;

namespace ivm {
namespace {

// Shared state for `TransferablePromise` and `TransferablePromiseHolder`
struct TransferablePromiseStateStruct {
	std::shared_ptr<Transferable> value;
	// This would be a good case for a unique_ptr version of RemoteHandle
	std::deque<RemoteHandle<Promise::Resolver>> waiting;
	bool did_throw = false;
	bool resolved = false;
};

using TransferablePromiseState = lockable_t<TransferablePromiseStateStruct>;

// Responsible for waiting for the promise in the isolate in which it was created
class TransferablePromiseHolder : public ClassHandle {
	public:
		explicit TransferablePromiseHolder(
			std::shared_ptr<TransferablePromiseState> state,
			Transferable::Options transfer_options
		) :
			state{std::move(state)}, transfer_options{transfer_options} {}

		TransferablePromiseHolder(const TransferablePromiseHolder&) = delete;

		~TransferablePromiseHolder() final {
			Save(true, [&]() {
				return std::make_shared<ExternalCopyError>(ExternalCopyError::ErrorType::Error, "Promise was abandoned");
			});
		}

		auto operator=(const TransferablePromiseHolder&) = delete;

		static auto Definition() -> v8::Local<v8::FunctionTemplate> {
			return MakeClass("PromiseHolder", nullptr);
		}

		void Accept(Local<Promise> promise) {
			auto state = promise->State();
			if (state == Promise::PromiseState::kPending) {
				auto isolate = Isolate::GetCurrent();
				auto context = isolate->GetCurrentContext();
				auto handle = This();
				promise = Unmaybe(promise->Then(context, Unmaybe(
					Function::New(context,
						FreeFunctionWithData<decltype(&Resolved), &Resolved>{}.callback, handle)
				)));
				Unmaybe(promise->Catch(context, Unmaybe(
					Function::New(context,
						FreeFunctionWithData<decltype(&Rejected), &Rejected>{}.callback, handle)
				)));
			} else {
				if (state == Promise::PromiseState::kFulfilled) {
					Resolved(*this, promise->Result());
				} else {
					Rejected(*this, promise->Result());
				}
			}
		}

		static void Resolved(TransferablePromiseHolder& that, Local<Value> value) {
			that.Save(false, [&]() {
				return Transferable::TransferOut(value, that.transfer_options);
			});
		}

	private:
		template <class Function>
		void Save(bool did_throw, Function callback) {
			std::shared_ptr<Transferable> resolved_value;
			auto pending_tasks = [&]() -> std::deque<RemoteHandle<Promise::Resolver>> {
				auto lock = state->write();
				if (!lock->resolved) {
					lock->resolved = true;
					FunctorRunners::RunCatchExternal(Isolate::GetCurrent()->GetCurrentContext(), [&]() {
						lock->value = callback();
						lock->did_throw = did_throw;
					}, [&](std::unique_ptr<ExternalCopy> error) {
						lock->value = std::move(error);
						did_throw = lock->did_throw = true;
					});
				}
				resolved_value = lock->value;
				return std::exchange(lock->waiting, {});
			}();
			for (auto& resolver : pending_tasks) {
				auto holder = resolver.GetIsolateHolder();
				holder->ScheduleTask(
					std::make_unique<ResolveTask>(std::move(resolver), resolved_value, did_throw),
					false, true);
			}
		}

		static void Rejected(TransferablePromiseHolder& that, Local<Value> value) {
			that.Save(true, [&]() {
				return ExternalCopy::CopyIfPrimitiveOrError(value);
			});
		}

		struct ResolveTask : Runnable {
			ResolveTask(RemoteHandle<Promise::Resolver> resolver, std::shared_ptr<Transferable> value, bool did_throw) :
				resolver{std::move(resolver)}, value{std::move(value)}, did_throw{did_throw} {}

			void Run() final {
				auto isolate = Isolate::GetCurrent();
				auto context = isolate->GetCurrentContext();
				auto resolver = Deref(this->resolver);
				if (did_throw) {
					Unmaybe(resolver->Reject(context, value->TransferIn()));
				} else {
					Unmaybe(resolver->Resolve(context, value->TransferIn()));
				}
			}

			RemoteHandle<Promise::Resolver> resolver;
			std::shared_ptr<Transferable> value;
			bool did_throw;
		};

		std::shared_ptr<TransferablePromiseState> state;
		Transferable::Options transfer_options;
};

// Internal ivm promise transferable
class TransferablePromise : public Transferable {
	public:
		TransferablePromise(Local<Promise> promise, Transferable::Options transfer_options) :
				state{std::make_shared<TransferablePromiseState>()} {
			MakeHolder(transfer_options).Accept(promise);
		}

		TransferablePromise(Local<Value> value, Transferable::Options transfer_options) :
				state{std::make_shared<TransferablePromiseState>()} {
			TransferablePromiseHolder::Resolved(MakeHolder(transfer_options), value);
		}

		auto TransferIn() -> Local<Value> final {
			auto isolate = Isolate::GetCurrent();
			auto context = isolate->GetCurrentContext();
			auto resolver = Unmaybe(Promise::Resolver::New(context));
			auto lock = state->write();
			if (lock->resolved) {
				if (lock->did_throw) {
					Unmaybe(resolver->Reject(context, lock->value->TransferIn()));
				} else {
					Unmaybe(resolver->Resolve(context, lock->value->TransferIn()));
				}
			} else {
				lock->waiting.emplace_back(resolver);
			}
			return resolver->GetPromise();
		}

	private:
		TransferablePromiseHolder& MakeHolder(Transferable::Options transfer_options) {
			transfer_options.promise = false;
			auto holder = ClassHandle::NewInstance<TransferablePromiseHolder>(state, transfer_options);
			auto object = ClassHandle::Unwrap<TransferablePromiseHolder>(holder);
			return *object;
		}

		std::shared_ptr<TransferablePromiseState> state;
};

} // anonymous namespace

namespace detail {

TransferableOptions::TransferableOptions(Local<Object> options, Type fallback) : fallback{fallback} {
	ParseOptions(options);
}

TransferableOptions::TransferableOptions(MaybeLocal<Object> maybe_options, Type fallback) : fallback{fallback} {
	Local<Object> options;
	if (maybe_options.ToLocal(&options)) {
		ParseOptions(options);
	}
}

void TransferableOptions::TransferableOptions::ParseOptions(Local<Object> options) {
	bool copy = ReadOption<bool>(options, "copy", false);
	bool externalCopy = ReadOption<bool>(options, "externalCopy", false);
	bool reference = ReadOption<bool>(options, "reference", false);
	if ((copy && externalCopy) || (copy && reference) || (externalCopy && reference)) {
		throw RuntimeTypeError("Only one of `copy`, `externalCopy`, or `reference` may be set");
	}
	if (copy) {
		type = Type::Copy;
	} else if (externalCopy) {
		type = Type::ExternalCopy;
	} else if (reference) {
		type = Type::Reference;
	}
	promise = ReadOption<bool>(options, "promise", false);
}

} // namespace detail

auto Transferable::OptionalTransferOut(Local<Value> value, Options options) -> std::unique_ptr<Transferable> {
	auto TransferWithType = [&](Options::Type type) -> std::unique_ptr<Transferable> {
		switch (type) {
			default:
				return nullptr;

			case Options::Type::Copy:
				return ExternalCopy::Copy(value);

			case Options::Type::ExternalCopy:
				return std::make_unique<ExternalCopyHandle::ExternalCopyTransferable>(ExternalCopy::Copy(value));

			case Options::Type::Reference:
				return std::make_unique<ReferenceHandleTransferable>(value);
		}
	};

	if (options.promise) {
		if (value->IsPromise()) {
			return std::make_unique<TransferablePromise>(value.As<Promise>(), options);
		} else {
			return std::make_unique<TransferablePromise>(value, options);
		}
	}

	switch (options.type) {
		case Options::Type::None: {
			if (value->IsObject()) {
				auto ptr = ClassHandle::Unwrap<TransferableHandle>(value.As<Object>());
				if (ptr != nullptr) {
					return ptr->TransferOut();
				}
			}
			auto result = ExternalCopy::CopyIfPrimitive(value);
			if (result) {
				return result;
			}
			return TransferWithType(options.fallback);
		}

		default:
			return TransferWithType(options.type);
	}
}

auto Transferable::TransferOut(Local<Value> value, Options options) -> std::unique_ptr<Transferable> {
	auto copy = OptionalTransferOut(value, options);
	if (!copy) {
		throw RuntimeTypeError("A non-transferable value was passed");
	}
	return copy;
}

} // namespace ivm
