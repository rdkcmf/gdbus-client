/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 Liberty Global B.V.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GDBusClient.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstdio> //for logging backup if rdk logger fails to initialize
#include <vector>
#include <map>
#include <set>
#include <string>
#include <functional>
#include <utility>
#include <algorithm>
#include <regex>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <gio/gio.h>

#define AT() __func__, __LINE__

#define verboseNonNull(p)   _verboseNonNull(__func__, __LINE__, (p), #p)
// verboseNonNull macro is a wrapper for _verboseNonNull function defined below


namespace gdbus_client {
    namespace converters {
        using namespace gdbus_client::GDBusType;

        // Marshallers and un-marshallers, to convert between D-Bus GVariant types
        // and underlying values in GDBusParam.
        // This list corresponds to the list of GDBusParam constructors declared
        // in GDBusClient.hpp.
        GVariant * marshal( TYPE_S,     const std::string&);
        GVariant * marshal( TYPE_I,     int32_t);
        GVariant * marshal( TYPE_U,     uint32_t);
        GVariant * marshal( TYPE_Y,     uint8_t);
        GVariant * marshal( TYPE_N,     int16_t);
        GVariant * marshal( TYPE_T,     uint64_t);
        GVariant * marshal( TYPE_B,     bool);
        GVariant * marshal( TYPE_D,     double);
        GVariant * marshal( TYPE_O,     const std::string&);
        GVariant * marshal( TYPE_V,     const std::string&);
        GVariant * marshal( TYPE_AS,    const str_arr_t &);
        GVariant * marshal( TYPE_AO,    const str_arr_t &);
        GVariant * marshal( TYPE_DICT,  const dict_t&);
        //GVariant * marshal( TYPE_DICT,  const dict_t&);       // Not implemented: not useful.
        //GVariant * marshal( TYPE_ATUP,  const tuple_arr_t &); // Not implemented: TYPE_ATUP is intended for out params only
        //GVariant * marshal( TYPE_ANY,   const std::string&);  // Not implemented: TYPE_ANY is intended for out params only
        bool unmarshal(     TYPE_S,     GVariant *, std::string&);
        bool unmarshal(     TYPE_I,     GVariant *, int32_t&);
        bool unmarshal(     TYPE_U,     GVariant *, uint32_t&);
        bool unmarshal(     TYPE_Y,     GVariant *, uint8_t&);
        bool unmarshal(     TYPE_N,     GVariant *, int16_t&);
        bool unmarshal(     TYPE_T,     GVariant *, uint64_t&);
        bool unmarshal(     TYPE_B,     GVariant *, bool&);
        bool unmarshal(     TYPE_D,     GVariant *, double&);
        bool unmarshal(     TYPE_O,     GVariant *, std::string&);
        bool unmarshal(     TYPE_V,     GVariant *, std::string&);
        bool unmarshal(     TYPE_AS,    GVariant *, str_arr_t&);
        bool unmarshal(     TYPE_AO,    GVariant *, str_arr_t&);
        bool unmarshal(     TYPE_DICT,  GVariant *, dict_t&);
        bool unmarshal(     TYPE_VDICT, GVariant *, dict_t&);
        bool unmarshal(     TYPE_ATUP,  GVariant *, tuple_arr_t&);
        bool unmarshal(     TYPE_ANY,   GVariant *, std::string&);
    }
}


namespace {

    struct call_t;
    struct param_t;
    struct proxy_t;

    struct obj_desc_t {                                     // D-Bus service descriptor
        std::string name, path, iface;

        static obj_desc_t fromName(const std::string &name) {
            const std::regex dot_regex{ "[.]" };            // the regex to convert obj_name to obj_path, '.' --> '/'
            return obj_desc_t{
                name,
                std::string("/") + std::regex_replace(name, dot_regex, "/"),
                name };
        }

        static obj_desc_t fromDesc(const gdbus_client::GDBusObjectDescriptor &d)
        {
            return obj_desc_t{ d.obj_name, d.obj_path, d.iface_name };
        }
    };


    thread_local const gdbus_client::GDBusCall* callUnderConstruction = nullptr;// Set in GDBusCall ctor and cleared in GDBusCall dtor.
                                                                                // Thread-local to avoid bogus interaction when initializing
                                                                                //      GDBusCall-s in different threads at the same time.
    bool logAssert( const char *func, unsigned line,
                    bool f, const std::string &err);

    template <typename P>
    P* _verboseNonNull( const char *func, unsigned line,    // Complains if p is null, but then passes it through.
                        P *p, const std::string &expr)      // func, line and expr are provided by the verboseNonNull macro.
    {                                                       // Used in "this should never happen" situations.
        logAssert(func, line, static_cast<bool>(p), expr);
        return p;
    }


    struct gerror_t {
        GError *err = nullptr;

        explicit operator GError**() { return &err; }

        bool verboseCheckNoErr(const char *func, unsigned line);
        void clear() {g_clear_error(&err); }
        ~gerror_t() { clear(); }

        enum errcode_t {
            NOERR,
            SERVICE_UNKNOWN,
            SERVER_DISCONNECT,
            ACCESS_DENIED,
            UNSPECIFIED };

        const std::map<std::pair<unsigned,int>, errcode_t> errorMap {
            {{G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN},  SERVICE_UNKNOWN},
            {{G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED},     SERVER_DISCONNECT},
            {{G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED},    ACCESS_DENIED},
        };

        errcode_t errType() const {
            return (err == nullptr) ?
                            errcode_t::NOERR  :
                    errorMap.count({err->domain, err->code})?
                            errorMap.at({err->domain, err->code}):
                            errcode_t::UNSPECIFIED;
        }


    };


    struct call_t {
        std::vector<param_t> params;                // in and out parameters that belong to this call
        obj_desc_t object;                          // target of the call
        std::string method;                         // the target method

        call_t() = default;
        call_t(call_t&&) = default;                 // Allow move ctor and forbid copy ctor implicitly

        call_t(obj_desc_t obj, std::string method)
            :   object{ std::move(obj) }, method{ std::move(method) }
        {
            verboseCheckNoErr(AT());
        }

        bool verboseCheckNoErr(const char *func, unsigned line);
    };


    // A D-Bus signal is represented as a call without input params
    // So far not used
    struct signal_t: call_t {
        explicit signal_t(obj_desc_t obj, std::string sig_name)
            : call_t{ std::move(obj), std::move(sig_name) }
        {}
    };


    //  call_storage_t keeps a map of the registered gdbus calls (of type call_t)
    //  and provides access to this list via its member functions add, get and remove,
    //  which are reentrant (protected by a shared mutex).
    //
    //  In addition to that, it monitors the reference count to the registered calls,
    //  and prints an error message in case if the reference count is not as expected
    //  during the lifetime of the gdbus calls. This helps to monitor incorrect multithreaded
    //  access to the GDBusCall class; see 'Usage Details' section in GDBusClient.hpp.

    struct call_storage_t {
        using call_ptr_t = std::shared_ptr<call_t>; // The registered calls are stored using shared pointers.
                                                    // The use_count of the pointer helps to detect simultaneous modification
                                                    //      of the output params from different threads.
        struct call_guard_t {   // call_guard_t is a thin wrapper for shared_ptr<call_t> used to
            call_ptr_t call;    //      check the value of use_count of the pointer with verboseCheckOwnership

            call_guard_t(call_guard_t &&g) noexcept :call{std::move(g.call)}
            { g.call.reset(); }

            explicit call_guard_t(call_ptr_t &&ptr) :call{ptr} {}

            ~call_guard_t() {
                if (!storage_destroyed) // if destroyed, there is only one owner left
                    verboseCheckOwnership(2, AT()); // normally, there are two owners of the call: this guard and the calls map.
            }
            bool verboseCheckOwnership( unsigned n_owners,
                                        const char *func, unsigned line);
        };


        std::mutex call_mutex;   // protect the calls map from simultaneous modification and access
        std::map<const gdbus_client::GDBusCall*, call_ptr_t> calls; // The map of the registered calls; shared between threads.
        static std::atomic_bool storage_destroyed;  // Prevents access to the destoryed call_storage_t from other threads.
                                                    // It is set to false only when the application exits.

        void add(const gdbus_client::GDBusCall* p, call_t && call) {
            if (verboseStateCheck(AT())) {          // Avoid accessing 'this->call_mutex' after the end of life of 'this'.
                                                    // The 'else' clause here should never happen as long as the threads
                                                    // that use this client are joined before application exit.
                std::lock_guard<std::mutex> lock{call_mutex};
                calls.emplace(p, std::make_shared<call_t>(std::move(call)));
            }
        }

        call_guard_t get(const gdbus_client::GDBusCall* p) {
            if (verboseStateCheck(AT())) {
                std::lock_guard<std::mutex> lock{call_mutex};
                call_guard_t guard{std::move(calls[p])};
                if (guard.verboseCheckOwnership(2, AT()))   // Protect against overlapped calls from different threads:
                    return std::move(guard);                //      there should be two owners of guard.call at this moment.
            }
            return call_guard_t{call_ptr_t()};              // Return an empty pointer on error; the caller must check it.
        }

        void remove(const gdbus_client::GDBusCall* p) {
            if (verboseStateCheck(AT())) {
                std::lock_guard<std::mutex> lock{call_mutex};
                calls.erase(p);
            }
        }
        ~call_storage_t() {
            storage_destroyed.store(true);
        }

        static bool verboseStateCheck(const char *func, unsigned line);
    }
    calls;


    // storage_destroyed monitors the lifetime of the call_storage_t instance
    // and is used to prevent access to the call_storage_t instance after it was
    // destroyed. This might happen in the situation when another (detached) thread
    // tries to access the storage while the process is shutting down. This should
    // not happen in production (other threads should be properly joined before
    // the destruction of statics begin), but is a typical scenario in tests.
    std::atomic_bool call_storage_t::storage_destroyed { false };

    struct param_t {
        const char *name;       // Kept for error reporting and introspection
        const char *type;       // Kept for error reporting and introspection

        std::function<GVariant*()>      marshal;    // The marshaller, to convert 'in' parameters into GVariant fields in D-Bus messages
        std::function<bool(GVariant *)> unmarshal;  // The unmarshaller, to decode 'out' parameter values from GVariant
                                                    // For each parameter instance, only one of those two is defined.
        std::function<void()>           cleanup;    // The cleanup function, to zero out the 'out' parameters on error.
        using PARAM_IN  = gdbus_client::GDBusDirection::PARAM_IN;
        using PARAM_OUT = gdbus_client::GDBusDirection::PARAM_OUT;
        using GDBusCall = gdbus_client::GDBusCall;


        template<typename ParamT, typename ValueT>                      // The ctor for 'in' parameters. Uses the marshaller only
        param_t( GDBusCall::GDBusParam<ParamT, PARAM_IN, ValueT> *par,  // and does not require the unmarshaller in compile time.
                const char *name, const char *type, ValueT v)           // 'v' is the default value of the parameter
            :   name(name), type(type), cleanup{ []{/*do nothing for 'in'*/} }
        {
            if (!verboseCheckNoErr(AT(), par, callUnderConstruction))   // ignore parameters that fail to satisfy pre-conditions
                return;
            par->value = std::move(v);                                  // set the initial value of the 'in' parameter
            marshal = [par]() { return                                  // Capture the pointer to the GDBusParam: it is used by the marshaller
                gdbus_client::converters::marshal(ParamT(), par->value); }; // to retrieve the input value.
        }

        template<typename ParamT,  typename ValueT>                     // The ctor for 'out' parameters. Only uses the unmarshaller;
        param_t( GDBusCall::GDBusParam<ParamT, PARAM_OUT, ValueT> *par, // does not need the marshaller, even in compile time.
                 const char *name, const char *type, ValueT v)
            :   name(name), type(type), cleanup{ [par]{par->value = {}; } }
        {
            if (!verboseCheckNoErr(AT(), par, callUnderConstruction))   // ignore parameters that fail to satisfy pre-conditions
                return;
            par->value = std::move(v);
            unmarshal = [par](GVariant *v) { return                     // Capture the pointer to the GDBusParam: it is used by the unmarshaller
                gdbus_client::converters::unmarshal(ParamT(), v, par->value); };// to store the output value.
        }

        void moveIntoCall() && {                                        // Consume this param_t instance and move it into the 'calls' map.
            auto call_guard = calls.get(callUnderConstruction);
            if (call_guard.call)
                call_guard.call->params.emplace_back(std::move(*this));
        }

        static bool verboseCheckNoErr(  const char *func, unsigned line, // Check whether param is a member field in a call struct
                                        const void *gdbus_par,
                                        const GDBusCall *call);

        bool verboseCheckMarshalled(    const char *func, unsigned line,
                                        GVariant *v) const;
        bool verboseCheckUnmarshalled(  const char *func, unsigned line,
                                        GVariant *v, bool result) const;

    };


    struct signal_storage_t {

        std::mutex signals_mutex;
        std::map<std::string,
                 std::vector<gdbus_client::GDBusSignal::callback_t>>
                signal_map;

        static const std::string key(const std::string &sender_name,
                         const std::string &sig_name)
        {
            return sender_name + " " + sig_name;
        }

        void add(const obj_desc_t &sender,
                 const std::string &signal_name,
                 const gdbus_client::GDBusSignal::callback_t &callback)
        {
            std::lock_guard<std::mutex> lock{signals_mutex};
            signal_map[key(sender.name, signal_name)].emplace_back(callback);
        }

        std::vector<gdbus_client::GDBusSignal::callback_t> get(
                const std::string &sender_name,
                const std::string &signal_name)
        {
            std::lock_guard<std::mutex> lock{signals_mutex};
            return signal_map[key(sender_name, signal_name)];
        }

    }
    signals;

    void onSignal(GDBusProxy *,
                  const char *,
                  const char *signal_name,
                  GVariant   *,
                  void * obj_name)
    {
        const char *sender_name = reinterpret_cast<const char*>(obj_name);
        auto callbacks = signals.get(sender_name, signal_name);
        for (const auto &callback: callbacks) {
            if (static_cast<bool>(callback)) {
                callback(sender_name, signal_name);
            }
        }
    }


    // g_context_switcher allows to temporary change the GLib default thread context
    struct gcontext_switcher_t {
        GMainContext *context = nullptr;
        gcontext_switcher_t(const gcontext_switcher_t &) = delete;
        gcontext_switcher_t();  // the constructor changes the current gcontext to the one (indirectly) returned from mainLoopInstance
        ~gcontext_switcher_t(); // the destructor restores the current gcontext, if changed in constructor
    };


    struct proxy_t {
        GDBusProxy *proxy = nullptr;
        std::string obj_name;

        proxy_t() = default;
        proxy_t(const proxy_t&) = delete;

        proxy_t& operator=(proxy_t &&q) noexcept {
            if (proxy != q.proxy) {         // This class maintains ownership of GDBusProxy * proxy;
                g_clear_object(&proxy);     // thus the assignment operator transfers the ownership.
                std::swap(proxy, q.proxy);  // The .proxy field of the original owner is cleared.
                obj_name = std::move(q.obj_name);
            }
            return *this;
        }

        explicit proxy_t(const obj_desc_t &obj) : obj_name(obj.name) {
            gcontext_switcher_t gcontext_switcher{}; // Push the thread-default context to make sure that
                                                     //     the callbacks of the proxy are dispatched in the
                                                     //     thread that runs waitAndProcessSignals (see).
                                                     // Also creates the custom main loop and context if they are not yet created.
            gerror_t err;
            proxy = g_dbus_proxy_new_for_bus_sync(                       // The proxy is created in the new thread-default context,
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,     //    so the signal callbacks are dispatched in the context set by
                obj.name.c_str(), obj.path.c_str(), obj.iface.c_str(),   //    the context switcher, which is the one bound to the main loop
                nullptr, static_cast<GError**>(err));                    //    created in mainLoopInstance.
                                                                         // Therefore, the callbacks are dispatched in the thread that calls
            if (err.verboseCheckNoErr(AT())) {                           //     waitAndProcessSignals that iterates the main loop/context.
                g_signal_connect(proxy,                                  // This way we avoid disturbing the application-default thread context,
                                 "g-signal",                             //    that is sometimes abused by other libraries that use D-Bus to dispatch
                                 G_CALLBACK(onSignal),                   //    their signals.
                                 const_cast<char*>(obj_name.c_str()));
            }
        }

        bool verboseCheckNoErr(const char *func, unsigned line) const;

        ~proxy_t() {
            g_clear_object(&proxy); // proxy == null is ok here
        }

        enum Policy { USE_EXISTING, RECREATE };

        static proxy_t& instanceFor(const obj_desc_t &obj,  // Reentrant
                                    const Policy policy)
        {
            static std::map<std::string, proxy_t> proxies;
            static std::mutex proxy_mutex;

            const std::string target =
                    obj.name + " " + obj.path + " " + obj.iface;

            std::lock_guard<std::mutex> lock{ proxy_mutex };
            if (policy == Policy::RECREATE || !proxies[target].proxy) {
                proxies[target] = proxy_t{obj};
            }
            return proxies[target];
        }
    };


    struct variant_holder_t: public std::vector<GVariant *> {
        bool adopt(GVariant *v) {
            if (v)
                push_back(g_variant_take_ref(v));
            return static_cast<bool>(v);
        }

        GVariant* to_tuple() {
            return g_variant_new_tuple( empty()? nullptr : &front(), size() );
        }

        ~variant_holder_t() {
            std::for_each(begin(), end(),
                [](GVariant *v){ if (v) g_variant_unref(v);}) ;
        }

        static variant_holder_t from_tuple(GVariant *tuple) {
            variant_holder_t vh;
            for (unsigned i = 0; i < g_variant_n_children(tuple); i++)
                vh.adopt(g_variant_get_child_value(tuple, i));
            return vh;
        }
    };

    // On the first invocation, create a new GLib context and event loop.
    // On subsequent invocations, check whether the loop received an exit signal,
    // and, if so, delete the context and the event loop and set their pointers
    // to null. Return the current value of the event loop pointer.
    GMainLoop* mainLoopInstance() {
        static GMainContext *context = g_main_context_new();    // create the new g-context on the first invocation
        static GMainLoop *loop = context?                       // create the new g-event-loop on the first invocation
                g_main_loop_new(context, true):
                nullptr;

        if (context) {
            g_main_context_unref(context);  // unref the context immediately; the loop holds another reference,
            context = nullptr;              //    so it is not destoyed here
        }

        if (loop && !g_main_loop_is_running(loop)) {
            g_main_loop_unref(loop);        // this also unrefs the main context, which is then destroyed
            loop = nullptr;
        }

        return loop;
    }

    GMainContext* mainContextOf(GMainLoop *loop) {
        return loop? g_main_loop_get_context(loop) : nullptr;
    }

    struct loop_timeout_t {
        GSource *timeout = nullptr;
        unsigned intervalMsec = 0;

        // Set a timeout to wake up the main loop approximately after the given number of milliseconds
        loop_timeout_t(unsigned interval_msec, GMainLoop *loop)  {
            if (!loop) {
                return;
            }
            intervalMsec = interval_msec;
            if (intervalMsec != gdbus_client::WAIT_FOREVER) {
                timeout = g_timeout_source_new(intervalMsec);
                g_source_set_callback(timeout, noOpHandler, nullptr, nullptr);
                g_source_attach(timeout, mainContextOf(loop));
                g_source_unref(timeout); // one more reference is kept internally by the context,
                                        //    therefore, the source is not destroyed here
            }
        }

        ~loop_timeout_t() {
            if (timeout) {
                g_source_destroy(timeout);
            }
            timeout = nullptr;
            intervalMsec = gdbus_client::WAIT_FOREVER;

        }
        static int noOpHandler(void *){ return G_SOURCE_CONTINUE; }
    };


    // g_context_switcher allows to temporary switch the GLib default thread context
    gcontext_switcher_t::gcontext_switcher_t() {    // the constructor changes the current gcontext to the one (indirectly) returned from mainLoopInstance
        context = mainContextOf(mainLoopInstance());
        if (context) {
            g_main_context_push_thread_default(context);
        }
    }
    gcontext_switcher_t::~gcontext_switcher_t() {   // the destructor restores the current gcontext, if changed in constructor
        if (context) {
            g_main_context_pop_thread_default(context);
        }
    }

    struct call_cleanup_guard_t {
        call_t &call;
        bool result = false;

        void setSuccess() { result = true; }
        ~call_cleanup_guard_t() {
            if (!result) {
                for (param_t &param: call.params) { // Loop over the output params of the call
                    param.cleanup();                // Clean up out params;
                }                                   //  for 'in' params this does nothing
            }
        }
    };
}


namespace gdbus_client {

    GDBusCall::GDBusCall(const GDBusObjectDescriptor &obj, const char *method) {
        callUnderConstruction = this;
        calls.add( this, call_t{ obj_desc_t::fromDesc(obj), method } );
    }

    GDBusCall::GDBusCall(const char *obj_name, const char *method) {
        callUnderConstruction = this;
        calls.add( this, call_t{ obj_desc_t::fromName(obj_name), method } );
    }

    GDBusCall::~GDBusCall() {
        calls.remove(this);
        if (callUnderConstruction == this)
            callUnderConstruction = nullptr;
    }

    bool GDBusCall::callSync() {    // Reentrant, although the values of out params of the call
                                    //      might be inconsistent.
        auto call_guard = calls.get(this);  // The call_guard is destroyed on return, unlocking the call data.
        if (!call_guard.call)       // Return false if calls.get returned an empty/dummy call.
            return false;           // The error is already logged when executing calls.get().

        call_t &call = *call_guard.call;

        call_cleanup_guard_t cleanup_guard{ call };     // Zero out the 'out' params of the call on failure

        variant_holder_t in_variants;   // Loop over the input params and marshal them into in_variants
        for (const param_t &in_param: call.params) {
            if (in_param.marshal) {     // include 'in' parameters only
                GVariant *in_variant = in_param.marshal();
                if (!in_param.verboseCheckMarshalled(AT(), in_variant) ||
                    !in_variants.adopt(verboseNonNull(in_variant)))
                    return false;
            }
        }

        GVariant *in_tuple = in_variants.to_tuple();    // The tuple to be put into the D-Bus message

        variant_holder_t tuples;                        // 'tuples' is used as a hook to unreference and destroy on return the variants it has adopted.
        tuples.adopt(in_tuple);                         // Keep a reference to in_tuple, to have it properly destroyed on return.

        GVariant *out_tuple = nullptr;
        gerror_t err;

        static const std::set<gerror_t::errcode_t> retriableErrors{
                gerror_t::SERVICE_UNKNOWN,
                gerror_t::SERVER_DISCONNECT
        };

        const int
            MAX_ATTEMPTS = 3,   // try to make the call up to three times
            WAIT_MS = 250;      // wait 250 ms before retrying
        for (int attempts = MAX_ATTEMPTS; !out_tuple && attempts; attempts--) {

            if (retriableErrors.count(err.errType())) {         // if we should retry on this type of error
                std::this_thread::sleep_for(std::chrono::milliseconds{WAIT_MS});// sleep before retying
            }
            else if (err.errType() != gerror_t::NOERR) {        // in case of other errors bail out immediately and do not retry
                break;
            }

            const proxy_t::Policy proxy_policy = (attempts == MAX_ATTEMPTS ?
                    proxy_t::USE_EXISTING:  // first encounter: use the pre-existing proxy, if possible
                    proxy_t::RECREATE);     // on retry, recreate the proxy

            proxy_t &proxy = proxy_t::instanceFor(call.object, proxy_policy);
            if (!proxy.verboseCheckNoErr(AT())) {
                return false;
            }
            err.clear();
            out_tuple = g_dbus_proxy_call_sync(
                    proxy.proxy, call.method.c_str(), in_tuple,
                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                    (GError **) err);
        }

        if (!err.verboseCheckNoErr(AT()) ||
            !tuples.adopt(verboseNonNull(out_tuple))) { // Keep a reference to out_tuple to destroy it on return.
            return false;                               // out_tuple is NULL on error
        }

        variant_holder_t out_variants = variant_holder_t::from_tuple(out_tuple);
        auto out_var_iter = out_variants.begin();

        for (param_t &out_param: call.params) { // Loop over the output params and unmarshal the response into them.
            if (out_param.unmarshal) {    // include 'out' parameters only

                if (out_var_iter == out_variants.end() ||
                    !verboseNonNull(*out_var_iter))
                    return false;

                if ( !out_param.verboseCheckUnmarshalled( AT(),
                    *out_var_iter, out_param.unmarshal(*out_var_iter) ) )
                    return false;

                out_var_iter++;
            }
            // Possibly, add a check here that out_var_iter reached out_variants.end(),
            //  but this will kill extensibility of the D-Bus API.
        }
        cleanup_guard.setSuccess();
        return true;
    }


    bool GDBusSignal::registerCallback(const char *sender_name,
                                       const char *signal_name,
                                       const GDBusSignal::callback_t &callback)
    {
        const obj_desc_t sender = obj_desc_t::fromName(sender_name);
        signals.add(sender, signal_name, callback);
        proxy_t &proxy = proxy_t::instanceFor(sender, proxy_t::USE_EXISTING);
        return proxy.verboseCheckNoErr(AT());
    }

    bool GDBusSignal::registerCallback(const GDBusObjectDescriptor &desc,
                                       const char *signal_name,
                                       const GDBusSignal::callback_t &callback)
    {
        const obj_desc_t sender{desc.obj_name, desc.obj_path, desc.iface_name};
        signals.add(sender, signal_name, callback);
        proxy_t &proxy = proxy_t::instanceFor(sender, proxy_t::USE_EXISTING);
        return proxy.verboseCheckNoErr(AT());
    }

    bool waitAndProcessSignals(unsigned wait_msec) {
        using namespace std::chrono;

        // wake up the main loop approx every (wait_msec) milliseconds
        loop_timeout_t timeout{ wait_msec, mainLoopInstance() };

        // iterate while the main loop is running and wait_msec is not elapsed
        for (auto t_end = steady_clock::now() + milliseconds(wait_msec);
                mainLoopInstance() &&
                (wait_msec == WAIT_FOREVER || t_end > steady_clock::now());)
        {
            GMainContext *context = mainContextOf(mainLoopInstance());
            if (context) {
                g_main_context_iteration(context, true);    // process signals and timers; block if none
            }
        }
        return mainLoopInstance() != nullptr;
    }

    void stopProcessingSignals() {  // idempotent
        GMainLoop *loop = mainLoopInstance();
        if (loop) {
            g_main_loop_quit(loop);
            mainLoopInstance(); // let the main loop process the quit signal
        }
    }
}


namespace { // Error reporting section

    bool gerror_t::verboseCheckNoErr(const char *func, unsigned line) {
        if (!err)
            return true;

        static const std::map<errcode_t, std::string> errMessages  {
            { errcode_t::ACCESS_DENIED,
                "D-Bus: access denied when trying to send, check policies" },
            { errcode_t::SERVICE_UNKNOWN,
                "D-Bus: unknown D-Bus object name, check if server is up" },
            { errcode_t::SERVER_DISCONNECT,
                "D-Bus: server disconnected in the middle of the call" },
            { errcode_t::UNSPECIFIED,
                "D-Bus: unspecified error" },
            { errcode_t::NOERR, "" },
        };

        const std::string message = (errMessages.count(errType())?
                errMessages.at(errType()) : std::string())
                + "\n[" + g_quark_to_string(err->domain) + ":"
                + std::to_string(err->code) + "]: " + std::string(err->message);
        return logAssert(func, line, false, message);
    }

    bool call_t::verboseCheckNoErr(const char *func, unsigned line) {
        bool good = true;

        good = logAssert( func, line,
                          static_cast<bool>(g_dbus_is_name(object.name.c_str())),
                          object.name + ": invalid dbus object name")
                && good;

        good = logAssert( func, line,
                          static_cast<bool>(g_variant_is_object_path(object.path.c_str())),
                          object.path + ": invalid dbus object path")
                && good;

        good = logAssert( func, line,
                          static_cast<bool>(g_dbus_is_interface_name(object.iface.c_str())),
                          object.iface + ": invalid dbus iface name")
               && good;

        good = logAssert( func, line,
                          static_cast<bool>(g_dbus_is_member_name(method.c_str())),
                          method + ": invalid dbus method")
               && good;

        return good;
    }

    bool call_storage_t::call_guard_t::verboseCheckOwnership(   // Check that the use_count of the call pointer
            unsigned n_owners, const char *func, unsigned line) //      is as expected.
    {
        const std::string call_info = call?
                (call->object.iface + ":" + call->method) : "";

        const unsigned n_users = (unsigned)call.use_count();
        return logAssert( func, line,
                n_users == n_owners || n_users == 0,
                call_info + ": the call should have " + std::to_string(n_owners)
                    + " users instead of " + std::to_string(call.use_count()));
    }

    bool call_storage_t::verboseStateCheck( const char *func,
                                            const unsigned line)
    {
        return logAssert( func, line, !storage_destroyed,
                "Detected access to GDBus Client after it was destroyed");
    }

    bool param_t::verboseCheckNoErr(const char *func, unsigned line,
                                    const void *gdbus_par,
                                    const GDBusCall *call)
    {
        const char *err =  "Error initializing a D-Bus parameter: ";
        static const unsigned MAX_CALL_BODY_SIZE = 4*1024;                  // The maximum size of the body of the GDBusCall descendant.

        const bool par_in_call =    gdbus_par >= call &&                    // Check that the given GDBusParam<> lies
                                    gdbus_par < call + MAX_CALL_BODY_SIZE;  // within the body of a Call instance. This is to warn about
                                                                            // GDBusParam instances that are not class members of some GDBusCall
        return  logAssert( func, line, static_cast<bool>(call),             // Verify that the GDBusCall instance has been already constructed.
                           std::string(err) + "no Call instance")
                &&  logAssert( func, line, par_in_call,
                               std::string(err) + "a stray parameter" );
    }

    bool param_t::verboseCheckMarshalled(   const char *func, unsigned line,
                                            GVariant *v) const
    {
        return logAssert(func, line, v != nullptr,
            std::string("Error marshalling a param ") + name + ": " + type
            + "; marshaller: " + std::to_string(static_cast<bool>(marshal)));
    }

    bool param_t::verboseCheckUnmarshalled( const char *func, unsigned line,
                                            GVariant *v, bool result) const
    {
        return logAssert(func, line, result,
            std::string("Error unmarshalling a param ") + name + ": " + type
            + "; input " + std::to_string( v != nullptr ));
    }

    bool proxy_t::verboseCheckNoErr(const char *func, unsigned line) const {
        return logAssert(func, line, proxy != nullptr,
                         std::string("No proxy for ") + obj_name);
    }

}


#define PARAM_CTOR(gtype, dir, value_type) \
    template<> GDBusCall::GDBusParam<gtype, dir, value_type> ::GDBusParam(const char *param_name, value_type v) \
    { param_t{ this, param_name, gdbus_type().gType, std::move(v) }.moveIntoCall(); }

namespace gdbus_client {
    using namespace GDBusType;

    // Definitions of the GDBusParam constructors decalred in GDBusClinet.hpp
    // These definitions should be matched by marshallers and unmarshallers.
    PARAM_CTOR(TYPE_S,      PARAM_IN,   std::string);
    PARAM_CTOR(TYPE_S,      PARAM_OUT,  std::string);
    PARAM_CTOR(TYPE_I,      PARAM_IN,   int);
    PARAM_CTOR(TYPE_I,      PARAM_OUT,  int);
    PARAM_CTOR(TYPE_U,      PARAM_IN,   unsigned);
    PARAM_CTOR(TYPE_U,      PARAM_OUT,  unsigned);
    PARAM_CTOR(TYPE_Y,      PARAM_IN,   unsigned char);
    PARAM_CTOR(TYPE_Y,      PARAM_OUT,  unsigned char);
    PARAM_CTOR(TYPE_N,      PARAM_IN,   int16_t);
    PARAM_CTOR(TYPE_N,      PARAM_OUT,  int16_t);
    PARAM_CTOR(TYPE_T,      PARAM_IN,   uint64_t);
    PARAM_CTOR(TYPE_T,      PARAM_OUT,  uint64_t);
    PARAM_CTOR(TYPE_B,      PARAM_IN,   bool);
    PARAM_CTOR(TYPE_B,      PARAM_OUT,  bool);
    PARAM_CTOR(TYPE_D,      PARAM_IN,   double);
    PARAM_CTOR(TYPE_D,      PARAM_OUT,  double);
    PARAM_CTOR(TYPE_O,      PARAM_IN,   std::string);
    PARAM_CTOR(TYPE_O,      PARAM_OUT,  std::string);
    PARAM_CTOR(TYPE_V,      PARAM_IN,   std::string);
    PARAM_CTOR(TYPE_V,      PARAM_OUT,  std::string);
    PARAM_CTOR(TYPE_AS,     PARAM_IN,   str_arr_t);
    PARAM_CTOR(TYPE_AS,     PARAM_OUT,  str_arr_t);
    PARAM_CTOR(TYPE_AO,     PARAM_IN,   str_arr_t);
    PARAM_CTOR(TYPE_AO,     PARAM_OUT,  str_arr_t);
    PARAM_CTOR(TYPE_DICT,   PARAM_IN,   dict_t);
    PARAM_CTOR(TYPE_DICT,   PARAM_OUT,  dict_t);
    //     No (TYPE_VDICT,  PARAM_IN,   dict_t) constructor: not needed for anything interesting.
    PARAM_CTOR(TYPE_VDICT,  PARAM_OUT,  dict_t);
    //     No (TYPE_ATUP,   PARAM_IN,   tuple_arr_t) ctor: TYPE_ATUP is not a real D-Bus type, thus cannot send it.
    PARAM_CTOR(TYPE_ATUP,   PARAM_OUT,  tuple_arr_t); // Still, decoding an out parameter to this fake type is possible
    //     No (TYPE_ANY,    PARAM_IN,   std::string) ctor: TYPE_ANY is not a real D-Bus type, thus cannot send it.
    PARAM_CTOR(TYPE_ANY,    PARAM_OUT,  std::string); // Still, decoding an out parameter to this fake type is possible

}


#ifdef RDK_LOGGER_ENABLED
#include <rdk_debug.h>

#else
namespace {

    int rdk_logger_init(const char *) { return -1; }
    enum { RDK_LOG_ERROR = 2 };
    void RDK_LOG(int, const char*, const char*,
                 const char*, const char*, unsigned, const char*) {}
}
#endif

namespace { // RDK logging

    bool have_logger() {
        static const char *LOGGER_CONFIG_FILE  = "/etc/debug.ini";
        static bool initialized = false;
        return  initialized ||                                              // If initialization of the rdk logger failed,
                (initialized = (rdk_logger_init(LOGGER_CONFIG_FILE) == 0)); // retry it each time this function is called.
    }

    bool logAssert(const char *func, unsigned line,
                   bool f, const std::string &err)
    {
        if (!f) {
            static const char *file =  strrchr("/" __FILE__, '/') + 1;
            if (have_logger()) {
                const char *RDK_MODULE_NAME  = "LOG.RDK.DBUS-CLIENT";
                RDK_LOG(RDK_LOG_ERROR, RDK_MODULE_NAME,
                        "[%s][%s][%u] [ERROR] %s",       // Follow the log format of dbus-client,
                        file, func, line,
                        err.c_str());  // as defined in debug.h
            }
            else
                printf("[%s][%s][%u] [ERROR] %s\n",
                       file, func, line, err.c_str());
        }
        return f;
    }


}
