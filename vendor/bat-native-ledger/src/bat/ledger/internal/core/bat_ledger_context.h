/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_VENDOR_BAT_NATIVE_LEDGER_SRC_BAT_LEDGER_INTERNAL_CORE_BAT_LEDGER_CONTEXT_H_
#define BRAVE_VENDOR_BAT_NATIVE_LEDGER_SRC_BAT_LEDGER_INTERNAL_CORE_BAT_LEDGER_CONTEXT_H_

#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "base/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "bat/ledger/ledger.h"
#include "bat/ledger/public/interfaces/ledger.mojom.h"

namespace ledger {

class LedgerImpl;
class LedgerClient;

// Represents a running instance of the BAT client engine. It serves as a
// loosely-coupled container for services, jobs and other components that are
// associated with, and owned by, the running instance. It also provides methods
// for logging and accessing environment-specific settings.
class BATLedgerContext : private base::SupportsUserData {
 public:
  // Base class for objects that are owned by an instance of BATLedgerContext.
  // The lifetime of the object is bounded by the lifetime of the context. As
  // such, it is generally safe for an object to access its containing context
  // by calling the |context()| accessor. Access to the containing context is
  // not allowed from the object's constructor or destructor.
  //
  // There are currently two primary object subtypes: components and jobs.
  // Components define a static |kContextKey| member. Instances are
  // initialized when first requested and are destroyed when the context is
  // destroyed.
  //
  //   class MyComponent : public BATLedgerContext::Object {
  //    public:
  //     static const char kContextKey[];
  //     ...
  //   };
  //
  // Job classes logically represent a single asynchronous operation and are
  // destroyed automatically when the operation completes.
  //
  //   class MyJob : public BATLedgerContext::Object {
  //    public:
  //     Future<int> GetFuture() { ... }
  //     void Start() { ... }
  //   };
  //
  // Jobs must implement a public |GetFuture()| method that returns an Future
  // object, and a |Start| method that begins the asynchronous operation.
  class Object : public base::SupportsUserData::Data {
   public:
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    ~Object() override;

   protected:
    Object();

    BATLedgerContext& context() { return *context_; }

   private:
    friend class BATLedgerContext;

    base::WeakPtr<BATLedgerContext> context_;
  };

  // NOTE: Values are based on the original logging design where each level from
  // 0 to 9 were assigned a specific subject matter. At some point in the future
  // these values can be renumbered.
  enum class LogLevel { kError = 0, kInfo = 1, kVerbose = 6, kFull = 9 };

  // Helper stream class returned from BATLedgerContext::Log* methods. Upon
  // destruction, the resulting string is sent to the client for logging.
  class LogStream {
   public:
    LogStream(BATLedgerContext* context,
              base::Location location,
              LogLevel log_level);

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    LogStream(LogStream&& other);
    LogStream& operator=(LogStream&& other);

    ~LogStream();

    template <typename T>
    LogStream& operator<<(const T& value) {
      stream_ << value;
      return *this;
    }

   private:
    BATLedgerContext* context_;
    base::Location location_;
    LogLevel log_level_;
    std::ostringstream stream_;
    bool moved_ = false;
  };

  explicit BATLedgerContext(LedgerClient* ledger_client);
  explicit BATLedgerContext(LedgerImpl* ledger_impl);

  ~BATLedgerContext() override;

  BATLedgerContext(const BATLedgerContext&) = delete;
  BATLedgerContext& operator=(const BATLedgerContext&) = delete;

  // Returns a pointer to the LedgerClient associated with this ledger context.
  // In general, this method should only be used by low-level components that
  // interact directly with the client. Before using this function in an
  // application-level component, check to see if there is an existing component
  // that wraps the low-level calls, and if not, consider creating one.
  LedgerClient* GetLedgerClient() const { return ledger_client_; }

  // Returns a pointer to the LedgerImpl associated with this ledger context.
  // This method is provided for backward-compatibility with existing code that
  // does not expose a context component and will be removed in the future.
  LedgerImpl* GetLedgerImpl() const { return ledger_impl_; }

  // Returns a reference to the context component of type T. T must expose a
  // static |kContextKey| member. The object will be created if necessary.
  //
  // Example:
  //   auto& my_component = context().Get<MyComponent>();
  template <typename T>
  T& Get() {
    static_assert(std::is_base_of<Object, T>::value,
                  "Get<T> requires that T is a subclass of "
                  "BATLedgerContext::Object");
    const void* key = T::kContextKey;
    if (T* ptr = static_cast<T*>(this->GetUserData(key)))
      return *ptr;

    std::unique_ptr<T> instance(new T());
    T* ptr = instance.get();
    this->SetUserData(key, std::move(instance));
    ptr->context_ = weak_factory_.GetWeakPtr();
    return *ptr;
  }

  template <typename T>
  void SetComponentForTesting(std::unique_ptr<T> instance) {
    static_assert(std::is_base_of<Object, T>::value,
                  "SetComponentForTesting<T> requires that T is a subclass of "
                  "BATLedgerContext::Object");
    this->SetUserData(T::kContextKey, std::move(instance));
  }

  // Starts a job and returns the Future associated with it. When the job
  // completes, the job instance is destroyed.
  //
  // Example:
  //   auto future = context().StartJob<MyJob>("hello");
  //   future.Then(...);
  template <typename T, typename... Args>
  auto StartJob(Args&&... args) {
    std::unique_ptr<T> instance(new T());
    T* ptr = instance.get();
    this->SetUserData(ptr, std::move(instance));

    ptr->context_ = weak_factory_.GetWeakPtr();
    ptr->Start(std::forward<Args>(args)...);
    auto future = ptr->GetFuture();

    using CompleteType = typename decltype(future)::CompleteType;
    return future.Map(
        base::BindOnce(BATLedgerContext::OnJobCompleted<CompleteType>,
                       weak_factory_.GetWeakPtr(), ptr));
  }

  // The Log* functions return a LogStream used to log messages to the client.
  // Log levels kError, kInfo, and kVerbose may be logged to disk by the client
  // and should not contain any information that would result in a breach of
  // security.
  //
  // Example:
  //   context().LogError(FROM_HERE) << "Something didn't work quite right";
  //
  // Since non-temporary objects are destroyed in LIFO order, be careful when
  // binding a LogStream to an lvalue, as it could result in a surprising
  // ordering of logging calls to the client.
  //
  //   LogStream stream1 = context().LogInfo(FROM_HERE);
  //   stream1 << "A";
  //   LogStream stream2 = context().LogInfo(FROM_HERE);
  //   stream2 << "B";
  //
  // In the example above, |stream2| is destroyed first, and therefore sends its
  // message to the client first.
  LogStream Log(base::Location location, LogLevel log_level);
  LogStream LogError(base::Location location);
  LogStream LogInfo(base::Location location);
  LogStream LogVerbose(base::Location location);
  LogStream LogFull(base::Location location);

  // Returns a const reference to a settings object of type T appropriate for
  // the current ledger environment.
  //
  // Example:
  //   struct MySettings {
  //     static const MySettings kDevelopment;
  //     static const MySettings kStaging;
  //     static const MySettings kProduction;
  //
  //     int magic_number;
  //   };
  //
  //   const MySettings MySettings::kDevelopment = {.magic_number = 1};
  //   const MySettings MySettings::kStaging = {.magic_number = 2};
  //   const MySettings MySettings::kProduction = {.magic_number = 3};
  //
  //   context().GetSettings<MySettings>().magic_number;
  template <typename T>
  const T& GetSettings() {
    switch (ledger::_environment) {
      case mojom::Environment::DEVELOPMENT:
        return T::kDevelopment;
      case mojom::Environment::STAGING:
        return T::kStaging;
      case mojom::Environment::PRODUCTION:
        return T::kProduction;
    }
  }

 private:
  template <typename T>
  static T OnJobCompleted(base::WeakPtr<BATLedgerContext> context,
                          void* job_key,
                          T value) {
    if (context)
      context->RemoveUserData(job_key);

    return value;
  }

  LedgerClient* ledger_client_;
  LedgerImpl* ledger_impl_ = nullptr;
  base::WeakPtrFactory<BATLedgerContext> weak_factory_{this};
};

}  // namespace ledger

#endif  // BRAVE_VENDOR_BAT_NATIVE_LEDGER_SRC_BAT_LEDGER_INTERNAL_CORE_BAT_LEDGER_CONTEXT_H_
