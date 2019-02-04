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

#ifndef GDBUS_CLIENT_GDBUSCLIENT_HPP
#define GDBUS_CLIENT_GDBUSCLIENT_HPP

#include <string>
#include <functional>
#include <map>
#include <vector>
#include <cstdint>


/* GDBusClient is a C++ wrapper around a subset of GLib g_dbus_ calls.
 *
 * Simplifies definition of D-Bus calls and conversion of input and output parameters of the calls
 * from and to STL-backed containers.
 *
 * Very basic support of D-Bus signals is provided.
 *
 */

namespace gdbus_client {

    // Types of parameters in D-Bus method calls
    namespace GDBusType {
        struct TYPE_S       { const char *gType = "s";      };      // D-Bus type 's', a string
        struct TYPE_I       { const char *gType = "i";      };      // D-Bus type 'i', a 32-bit int
        struct TYPE_U       { const char *gType = "u";      };      // D-Bus type 'u', a 32-bit unsigned int
        struct TYPE_Y       { const char *gType = "y";      };      // D-Bus type 'y', an 8-bit unsigned int
        struct TYPE_N       { const char *gType = "n";      };      // D-Bus type 'n', a 16-bit signed int
        struct TYPE_T       { const char *gType = "t";      };      // D-Bus type 't', a 64-bit unsigned int
        struct TYPE_B       { const char *gType = "b";      };      // D-Bus type 'b', a boolean
        struct TYPE_D       { const char *gType = "d";      };      // D-Bus type 'd', a floating point value
        struct TYPE_O       { const char *gType = "o";      };      // D-Bus type 'o', an object path
        struct TYPE_V       { const char *gType = "v";      };      // D-Bus type 'v', a variant type; exact type unknown
        struct TYPE_AS      { const char *gType = "as";     };      // D-Bus composite type 'as', an array of strings
        struct TYPE_AO      { const char *gType = "ao";     };      // D-Bus composite type 'ao', an array of object paths
        struct TYPE_DICT    { const char *gType = "a{ss}";  };      // D-Bus composite type 'a{ss}', an array of key-value (string-string) entities
        struct TYPE_VDICT   { const char *gType = "a{sv}";  };      // D-Bus composite type 'a{sv}', an array of variadic key-value (string-variant) entities
        struct TYPE_ATUP    { const char *gType = "a(*)";   };      // A synthetic type, an array of structs
        struct TYPE_ANY     { const char *gType = "*";   };         // A synthetic type, to decode and print arbitrary output parameters
    }

    // Direction of parameters in D-Bus method calls
    namespace GDBusDirection {
        struct PARAM_IN  {};
        struct PARAM_OUT {};
    }

    // A descriptor of a D-Bus object used in the second ctor of GDBusCall
    struct GDBusObjectDescriptor {
        const char *obj_name;       // The unique or well-known name, e.g. org.freedesktop.resolve1
        const char *obj_path;       // The D-Bus object path, e.g. /org/freedesktop/resolve1
        const char *iface_name;     // The interface name, e.g. org.freedesktop.resolve1.Manager
    };
    /* -------- Overview --------
     *
     * GDBusCall and GDBusParam structs defined below are intended to define and make D-Bus calls.
     * The usage is:
     *
     * 1. Define a D-Bus call as follows.
     *
     *      using gdbus_client::GDBusCall;
     *      using namespace gdbus_client::GDBusType;      // for TYPE_S, TYPE_I, etc.
     *      using namespace gdbus_client::GDBusDirection; // for PARAM_IN, PARAM_OUT
     *
     *      struct GetResourceIds: GDBusCall
     *      {
     *          GetResourceIds(): GDBusCall("com.lgi.rdk.utils.networkconfig1.restricted",
     *                                      "GetResourceIds") {}
     *          GDBusParam<TYPE_S,  PARAM_IN,   std::string>                resourceType{"resourceType"};
     *          GDBusParam<TYPE_I,  PARAM_OUT,  int>                        status      {"status"};
     *          GDBusParam<TYPE_U,  PARAM_OUT,  unsigned>                   count       {"count"};
     *          GDBusParam<TYPE_AS, PARAM_OUT,  std::vector<std::string>>   resourceIds {"resourceIds"};
     *      };
     *
     *      This code purely defines the structure of the D-Bus message and reply.
     *
     *      -- The target of the D-Bus call --
     *      The GDBusCall constructor receives the D-Bus object name and the target method. The object path
     *      and interface name are assumed to be the same as the given object name. If object path and
     *      interface name are different, use the other GDBusCall constructor and provide it with an instance
     *      of GDBusObjectDescriptor.
     *
     *      -- Types of the parameters --
     *      The D-Bus types known to GDBusClient are defined in GDBusType namespace. The D-Bus type of the
     *      parameter is the first template argument of GDBusParam declaration. It should correspond to the
     *      third template argument, which is the underlying C++ type holding the actual value. For the list
     *      of the supported holder types look at the list of GDBusParam constructors at the end of this
     *      header file.
     *
     * 2. Instantiate the D-Bus call class.
     *
     *      static GetResourceIds getResourceIds;
     *
     *      Each call struct might be instantiated one or multiple times.
     *
     * 3. Set the values of input parameters.
     *
     *      getResourceIds.resourceType.value = "dhcpv4";
     *
     * 4. Make the call.
     *
     *      const bool success = getResourceIds.callSync();
     *      if (success)
     *          std:cout << "Got " << resourceIds.value.size() << " resource ids";
     *
     *      callSync serializes the values of input parameters into a D-Bus message, waits for reply and
     *      de-serializes the reply into the output parameter values.
     *
     *      If callSync() method returned true, the values of output params contain the D-Bus reply.
     *      The subsequent invocations of callSync will overwrite these data.
     *
     */

    // GDBusCall is used as a base class for concrete call implementations.
    // This struct should be inherited, and cannot be instantiated.
    struct GDBusCall {

        // GDBusParam defines a parameter of a D-Bus call. Do not create a standalone
        // instance of this struct.
        template<typename Dbus_ParamType, typename Dir, typename ValueT>
        struct GDBusParam {
            ValueT value = {};          // the value of the input or output parameter
            explicit GDBusParam(const char *param_name, ValueT v = {});
            using gdbus_type = Dbus_ParamType;
        };

        // callSync makes the D-Bus call and handles serialization and de-serialization of
        // the call parameters.
        virtual bool callSync();

        virtual ~GDBusCall();   // safe to inherit

    protected:                  // inherit only; do not create instances

        // GDBusCall constructor sets the target of the call and binds calls parameters.
        // Use this constructor when object name, path and interface name are the same.
        GDBusCall(const char *obj_name, const char *method);

        // Provide GDBusObjectDescriptor if object name or path or interface name are
        // different from each over.
        GDBusCall(const GDBusObjectDescriptor &desc, const char *method);
    };


    // GDBusSignal is used as a base class for concrete signal implementations.
    // This struct should be inherited, and cannot be instantiated.
    struct GDBusSignal {

        using callback_t = std::function<void(const char *sender_name,
                                              const char *signal_name)>;

        // If tge signal does not have a body, just call one of registerCallback
        // static member functions, providing the proper source and name of the
        // signal; there is no need to subclass GDBusSigbal.
        static bool registerCallback(const char *obj_name,
                                     const char *signal_name,
                                     const callback_t &callback);

        // Provide GDBusObjectDescriptor if object name or path or interface
        // name are different from each over.
        static bool registerCallback(const GDBusObjectDescriptor &desc,
                                     const char *signal_name,
                                     const callback_t &callback);
    };


    // waitAndProcessSignals initializes (on the first invocation) and iterates
    //    the event loop that dispatches the signals registered via
    //    GDBusSignal::registerCallback.
    //
    // If signal processing is needed, iterate this function in an loop, until
    //    it returns false. false is returned when stopProcessingSignals() is
    //    called, or after the GLib contexts and main loops are destroyed on
    //    process exit.
    //
    // The argument, wait_msec, defines when this function shall return. Before
    //    return, it will be looping internally and processing incoming signals,
    //    or, if there a no signals or other reasons to wake up, it shall sleep.
    //
    // A typical usage is to run this function in a thread like this:
    //      while (waitAndProcessSignals(1000)) {
    //          if (signalReceived || timeout) doSomething();
    //          LOG("Heartbeat");
    //      }
    //
    const unsigned WAIT_FOREVER = -1u;
    bool waitAndProcessSignals(unsigned wait_msec);

    // stopProcessingSignals forces de-initialization of the internal GLib objects
    // created to support the event dispatching loop used by the signals.
    //
    // This is required if this .so library is loaded and unloaded dynamically,
    // while the main process continues execution.
    //
    // Otherwise, calling this is not necessary, because GLib will destroy
    // everything when destroying its static data, and the OS will clean up the
    // memory of the whole process anyway.
    void stopProcessingSignals();


    /* -------- Usage Details --------
     *
     * -- GDBusCall and D-Bus connection proxies --
     *
     * No connection to D-Bus is attempted unless GDBusCall::callSync is invoked. This is when this client
     * tries to discover the system bus and create a proxy for the given object, using its name, path and
     * interface name. If this attempt fails, e.g. because the target D-Bus object is not yet available on
     * the bus because the remote server is not ready, the client will retry to create the D-Bus proxy on
     * the next callSync invocation.
     *
     * If callSync succeeds in creating the object proxy but fails when calling it, the proxy is kept for
     * the next time.
     *
     * Multiple GDBusCall objects share, if possible, the same proxy and the same connection internally.
     *
     *
     * -- Multithreading --
     *
     * The client may be used in a multi-threaded environment.
     *
     * It is an error to use the same instance of a GDBusCall descendant from multiple threads, because
     * the results of the call might be written simultaneously to its members that are representing output
     * parameters, and the overall result would be inconsistent. The simultaneous use of callSync method is
     * detected by the GDBusCall class, and the subsequent call might fall.
     *
     * Avoid destroying GDBusCall when the call (made with the same class instance) is still in progress
     * in another thread.
     *
     * Therefore, if there are two threads that use the same GDBusCall child class, create the instances of
     * this class as automatic variables on stack, or, if they are class members or static variables, make
     * them thread_local. The last resort is to use an explicit synchronisation with a mutex.
     *
     *
     * -- Using std::move on the call parameters --
     *
     * When accessing the .value field of a GDBusParam class, feel free to steal the value of an output
     * parameter using std::move(call.param.value) to minimize the overhead.
     *
     * The same is applicable for input parameters as well: use call.param.value = std::move(data).
     *
     */


    // GDBusVariant is a wrapper on top of a const variant class, intended to hide
    // the implementation details of the variant. It is instantiated by some unmarshalling
    // functions. GDBusVariant is copyable.
    struct GDBusVariant {
        GDBusVariant();
        GDBusVariant(const GDBusVariant&);
        ~GDBusVariant();
        GDBusVariant& operator=(const GDBusVariant &);

        // getT functions retrieve the contents of the variant. In case of success,
        // the result argument is set to true. In case of failure, it is set to false
        // and the getT function returns 0 or "".
        int         getInt(bool &result) const;
        bool        getBool(bool &result) const;
        double      getDouble(bool &result) const;
        std::string getString(bool &result) const;
        std::string getVariant(bool &result) const;     // for future use

        // return the variant contents printed to a string
        std::string print() const;
    };


    // GDBusTuple is a helper class that stores the contents of an unmarshalled
    // D-Bus tuple.
    // A typycal usage is as follows:
    //
    //   1. Define a class inherited from GDBusTuple and use its internal classes
    //      (pint, pdouble, pbool, pstrings) to describe the contents of the tuple.
    //      E.g. for a D-Bus tuple "(si)" define
    //          struct SITuple: GDBusTuple {
    //              pstring name;
    //              pint    value;
    //          };
    //
    //   2. Get the contents of the tuple as a vector of GDBusVariant-s from one
    //      of the fields of a GDBusCall structure, and assign it to the derived
    //      structure SITuple:
    //
    //      std::vector<GDBusVariant> &array = call.siTuples.value[0];
    //      SITuple tuple;
    //      bool result = tuple.assign(array);
    //
    //   3. If the result returned by GDBusTuple::assign is true, you can use the
    //      fields of the SITuple instance:
    //          if (result) {
    //              std::cout << tuple.name << "=" << tuple.value << std::endl;
    //              int value = tuple.value;
    //          }
    //  For a practical example of GDBusTuple usage check
    //      onemw-src/av/sessionmanager/examples/testapp/sessionmgrTest.cpp

    struct GDBusTuple {

        // Construct the tuple from the array of GDBusVariant-s.
        // On success, initializes all the ptype-valued member in the derived class
        // and returns true. If conversion failed, false is returned; the values of
        // ptype fields are inconsistent in this case.
        bool assign(const std::vector<GDBusVariant> &variants);

        // Pretty-prints the contents of tuple
        std::string toString() const;

        // field is a helper class; normally should not be instantiated
        struct field { field(); field(const field&); ~field(); };

        // The ptype fields are wrappers for the int, double bool and string types,
        // used to represent D-Bus types i, f, b, s. Declare the members of type pint,
        // pdouble, pbool, pstring in the struct derived from the GDBusTuple to
        // describe the contents of the tuple.
        // The classes define the conversion operators to the corresponding type.
        // When you need to printf one of those values, convert it to the target type
        // explicitly, e.g. printf("%lf", (double)tuple.dbl);
        struct pint    :field { pint();    operator int()         const; };
        struct pdouble :field { pdouble(); operator double()      const; };
        struct pbool   :field { pbool();   operator bool()        const; };
        struct pstring :field { pstring(); operator std::string() const; };

        GDBusTuple(const GDBusTuple &tuple) noexcept;
        GDBusTuple& operator=(const GDBusTuple &tuple);
    protected:
        GDBusTuple();   // Do not create instances of this class; inherit it instead
        ~GDBusTuple();
    };


    using dict_t        = std::map<std::string, std::string>;
    using str_arr_t     = std::vector<std::string>;
    using tuple_arr_t   = std::vector<std::vector<GDBusVariant>>;
    using namespace GDBusDirection;
    using namespace GDBusType;

    /* The constructors for various GDBusParam types */

    template<> GDBusCall::GDBusParam<TYPE_S,    PARAM_IN,   std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_S,    PARAM_OUT,  std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_I,    PARAM_IN,   int>            ::GDBusParam(const char*, int);
    template<> GDBusCall::GDBusParam<TYPE_I,    PARAM_OUT,  int>            ::GDBusParam(const char*, int);
    template<> GDBusCall::GDBusParam<TYPE_U,    PARAM_IN,   unsigned>       ::GDBusParam(const char*, unsigned);
    template<> GDBusCall::GDBusParam<TYPE_U,    PARAM_OUT,  unsigned>       ::GDBusParam(const char*, unsigned);
    template<> GDBusCall::GDBusParam<TYPE_Y,    PARAM_IN,   unsigned char>  ::GDBusParam(const char*, unsigned char);
    template<> GDBusCall::GDBusParam<TYPE_Y,    PARAM_OUT,  unsigned char>  ::GDBusParam(const char*, unsigned char);
    template<> GDBusCall::GDBusParam<TYPE_N,    PARAM_IN,   int16_t>        ::GDBusParam(const char*, int16_t);
    template<> GDBusCall::GDBusParam<TYPE_N,    PARAM_OUT,  int16_t>        ::GDBusParam(const char*, int16_t);
    template<> GDBusCall::GDBusParam<TYPE_T,    PARAM_IN,   uint64_t>       ::GDBusParam(const char*, uint64_t);
    template<> GDBusCall::GDBusParam<TYPE_T,    PARAM_OUT,  uint64_t>       ::GDBusParam(const char*, uint64_t);
    template<> GDBusCall::GDBusParam<TYPE_B,    PARAM_IN,   bool>           ::GDBusParam(const char*, bool);
    template<> GDBusCall::GDBusParam<TYPE_B,    PARAM_OUT,  bool>           ::GDBusParam(const char*, bool);
    template<> GDBusCall::GDBusParam<TYPE_D,    PARAM_IN,   double>         ::GDBusParam(const char*, double);
    template<> GDBusCall::GDBusParam<TYPE_D,    PARAM_OUT,  double>         ::GDBusParam(const char*, double);
    template<> GDBusCall::GDBusParam<TYPE_O,    PARAM_IN,   std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_O,    PARAM_OUT,  std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_V,    PARAM_IN,   std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_V,    PARAM_OUT,  std::string>    ::GDBusParam(const char*, std::string);
    template<> GDBusCall::GDBusParam<TYPE_AS,   PARAM_IN,   str_arr_t>      ::GDBusParam(const char*, str_arr_t);
    template<> GDBusCall::GDBusParam<TYPE_AS,   PARAM_OUT,  str_arr_t>      ::GDBusParam(const char*, str_arr_t);
    template<> GDBusCall::GDBusParam<TYPE_AO,   PARAM_IN,   str_arr_t>      ::GDBusParam(const char*, str_arr_t);
    template<> GDBusCall::GDBusParam<TYPE_AO,   PARAM_OUT,  str_arr_t>      ::GDBusParam(const char*, str_arr_t);
    template<> GDBusCall::GDBusParam<TYPE_DICT, PARAM_IN,   dict_t>         ::GDBusParam(const char*, dict_t);
    template<> GDBusCall::GDBusParam<TYPE_DICT, PARAM_OUT,  dict_t>         ::GDBusParam(const char*, dict_t);
    // No spec here for  (GDBusParam<TYPE_VDICT,PARAM_IN,   dict_t>): not implemented as an input parameter
    template<> GDBusCall::GDBusParam<TYPE_VDICT,PARAM_OUT,  dict_t>         ::GDBusParam(const char*, dict_t);
    // No spec here for  (GDBusParam<TYPE_ATUP, PARAM_IN,   tuple_arr_t>): TYPE_ATUP is not a real DBUS type; cannot be an input param
    template<> GDBusCall::GDBusParam<TYPE_ATUP, PARAM_OUT,  tuple_arr_t>    ::GDBusParam(const char*, tuple_arr_t);
    // No spec here for  (GDBusParam<TYPE_ANY, PARAM_IN,   tuple_arr_t>): TYPE_ANY is not a real DBUS type; cannot be an input param
    template<> GDBusCall::GDBusParam<TYPE_ANY,  PARAM_OUT,  std::string>    ::GDBusParam(const char*, std::string);

}


#endif
