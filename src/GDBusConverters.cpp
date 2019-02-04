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

#include <gio/gio.h>
#include <string>
#include <map>
#include <mutex>
#include <utility>
#include <algorithm>
#include "GDBusClient.hpp"

#include <iostream>

namespace {

    struct variant_t {
        GVariant *gv = nullptr;

        operator bool() { return gv != nullptr; }

        variant_t(GVariant *g) : gv{g} {}

        variant_t() = default;

        variant_t(const variant_t &v) : gv{v.gv} {
            if (gv) {
                g_variant_ref(gv);
            }
        }
        variant_t(variant_t && v) noexcept {
            unref();
            std::swap(gv, v.gv);
        }
        ~variant_t() { unref(); }
        void unref() {
            if (gv)  {
                g_variant_unref(gv);
            }
            gv = nullptr;
        }
    };

    struct variants_map_t {
        using key_t = const gdbus_client::GDBusVariant*;

        std::map<key_t, variant_t> variants;
        std::mutex mutex;

        void insert(key_t key, variant_t &&v) {
            std::lock_guard<std::mutex> lock{ mutex };
            variants.emplace(key, std::move(v));
        }

        variant_t at(key_t key) {
            std::lock_guard<std::mutex> lock{ mutex };
            return variants.count(key)? variants.at(key) : variant_t{};
        }

        void erase(const key_t &key) {
            std::lock_guard<std::mutex> lock{ mutex };
            variants.erase(key);
        }
    }
    variants;

}

namespace gdbus_client {


GDBusVariant::GDBusVariant()  {
//    variants.insert(this, variant_t{});
}
GDBusVariant::~GDBusVariant()  {
    variants.erase(this);
}
GDBusVariant::GDBusVariant(const GDBusVariant &v) {
    variants.insert(this, variants.at(&v));
}

GDBusVariant& GDBusVariant::operator=(const GDBusVariant &v) {
    variants.erase(this);
    variants.insert(this, variants.at(&v));
    return *this;
}


int GDBusVariant::getInt(bool &result) const {
    int ret = 0;
    result = false;
    if (variant_t v = variants.at(this)) {
        if (g_variant_is_of_type(v.gv, G_VARIANT_TYPE_INT32)) {
            ret = g_variant_get_int32(v.gv);
            result = true;
        }
    }
    return ret;
}

bool GDBusVariant::getBool(bool &result) const {
    bool ret = false;
    result = false;
    if (variant_t v = variants.at(this)) {
        if (g_variant_is_of_type(v.gv, G_VARIANT_TYPE_BOOLEAN)) {
            ret = (g_variant_get_boolean(v.gv) != 0);
            result = true;
        }
    }
    return ret;
}

double GDBusVariant::getDouble(bool &result) const {
    double ret = 0.0;
    result = false;
    if (variant_t v = variants.at(this)) {
        if (g_variant_is_of_type(v.gv, G_VARIANT_TYPE_DOUBLE)) {
            ret = g_variant_get_double(v.gv);
            result = true;
        }
    }
    return ret;
}

std::string GDBusVariant::getString(bool &result) const {
    std::string ret;
    result = false;
    if (variant_t v = variants.at(this)) {
        if (g_variant_is_of_type(v.gv, G_VARIANT_TYPE_STRING)) {
            ret = g_variant_get_string(v.gv, nullptr);
            result = true;
        }
    }
    return ret;
}

std::string GDBusVariant::print() const {
    std::string ret;
    if (variant_t v = variants.at(this)) {
        char *s = g_variant_print(v.gv, false);
        ret = s;
        g_free(s);
    }
    return ret;
}

}


namespace {
    using gdbus_client::GDBusTuple;
    using gdbus_client::GDBusVariant;
    using getter_t = std::string(*)(const GDBusVariant&, bool&);
    using handle_t = const void*;

    struct field_ptr { const GDBusTuple * pt; getter_t getter; };
    std::map<handle_t, field_ptr> fields;
    bool unusedResult;


    struct tuple_t {
        std::vector<GDBusVariant> vars;
        std::vector<getter_t> getters;
        std::vector<handle_t > handles;

        tuple_t() = default;

        tuple_t(const tuple_t &t) {
            vars = t.vars;
        }

        unsigned newField(handle_t fh, getter_t getter) {
            handles.push_back(fh);
            getters.push_back(getter);
            return static_cast<unsigned >(getters.size() - 1);
        }

        GDBusVariant getVarByHandle(handle_t &fh) {
            const auto i = std::find(handles.begin(), handles.end(), fh);
            const auto pos = std::distance(handles.begin(), i);
            return pos >= 0 && pos < vars.size()? vars[pos] : GDBusVariant{};
        }

        std::string toString() const {
            if (vars.size() != getters.size()) {
                return "";
            }
            std::string str;
            for (unsigned i = 0; i < vars.size(); i++) {
                bool res = false;
                str += (str.empty() ? "" : " ");
                str += "<" + getters[i](vars[i], res) + ">";
                if (!res) {
                    return std::string();
                }
            }
            return std::string("(") + str + ")";
        }

        bool assign(const std::vector<GDBusVariant> &variants) {
            vars = variants;
            return !toString().empty();
        }
    };

    thread_local GDBusTuple *curTuple = nullptr;
    std::map<const GDBusTuple*, tuple_t> tuples;

    std::string strInt(   const GDBusVariant &v, bool &res) { return std::to_string(v.getInt(res));     }
    std::string strDouble(const GDBusVariant &v, bool &res) { return std::to_string(v.getDouble(res));  }
    std::string strBool(  const GDBusVariant &v, bool &res) { return std::to_string(v.getBool(res));    }
    std::string strString(const GDBusVariant &v, bool &res) { return v.getString(res);                  }



    void addField(handle_t fh, const GDBusTuple* ptuple, getter_t getter) {
        fields[fh] = field_ptr{ ptuple, getter };
        tuples[ptuple].newField(fh, getter);
    }

    void delField(handle_t fh) { fields.erase(fh); }

    GDBusVariant getField(handle_t fh) {
        if (!fields.count(fh) || !tuples.count(fields[fh].pt)) {
            return GDBusVariant{};
        }
        tuple_t & tuple = tuples[fields[fh].pt];
        return tuple.getVarByHandle(fh);
    }
}

namespace gdbus_client {

GDBusTuple::GDBusTuple() {
    curTuple = this;
    tuples.erase(this);
}

GDBusTuple::~GDBusTuple() {
    tuples.erase(this);
    curTuple = nullptr;
}

GDBusTuple::GDBusTuple(const GDBusTuple &tuple) noexcept {
    curTuple = this;
    tuples.erase(this);
    tuples.emplace(this, tuples[&tuple]);
}

GDBusTuple& GDBusTuple::operator=(const GDBusTuple &tuple) {
    curTuple = nullptr;
    tuples[this].assign(tuples[&tuple].vars);
    return *this;
}
bool GDBusTuple::assign(const std::vector<GDBusVariant> &variants) {
    return tuples[this].assign(variants);
}
std::string GDBusTuple::toString() const {
    return tuples[this].toString();
}

GDBusTuple::field::field() {}
GDBusTuple::field::~field() { delField(this); }
GDBusTuple::field::field(const GDBusTuple::field &f) {
    addField(this, curTuple, fields[&f].getter);
}
GDBusTuple::pint    ::pint()    { addField(this, curTuple, &strInt);    }
GDBusTuple::pdouble ::pdouble() { addField(this, curTuple, &strDouble); }
GDBusTuple::pbool   ::pbool()   { addField(this, curTuple, &strBool);   }
GDBusTuple::pstring ::pstring() { addField(this, curTuple, &strString); }

GDBusTuple::pint    ::operator int()        const { return getField(this).getInt(unusedResult);    }
GDBusTuple::pdouble ::operator double()     const { return getField(this).getDouble(unusedResult); }
GDBusTuple::pbool   ::operator bool()       const { return getField(this).getBool(unusedResult);   }
GDBusTuple::pstring ::operator std::string()const { return getField(this).getString(unusedResult); }



namespace converters {

using std::string;
using namespace GDBusType;


GVariant * marshal(const TYPE_S, const string &s) {
    return g_variant_new_string(s.c_str());
}

bool unmarshal(const TYPE_S, GVariant *gv, string &s) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_STRING) ?
                (s.assign(g_variant_get_string(gv, nullptr) ?: ""), true) :
                false;
}

GVariant * marshal(const TYPE_I, const int32_t i) {
    return g_variant_new_int32(i);
}

bool unmarshal(const TYPE_I, GVariant *gv, int32_t &i) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_INT32) ?
                (i = g_variant_get_int32(gv)), true:
                false;
}

GVariant * marshal(const TYPE_U, const uint32_t i) {
    return g_variant_new_uint32(i);
}

bool unmarshal(const TYPE_U, GVariant *gv, uint32_t &i) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_UINT32) ?
            (i = g_variant_get_uint32(gv)), true:
            false;
}

GVariant * marshal(const TYPE_Y, const uint8_t b) {
    return g_variant_new_byte(b);
}

bool unmarshal(const TYPE_Y, GVariant *gv, uint8_t &b) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_BYTE) ?
            (b = g_variant_get_byte(gv)), true:
            false;
}

GVariant * marshal(const TYPE_N, const int16_t i) {
    return g_variant_new_int16(i);
}

bool unmarshal(const TYPE_N, GVariant *gv, int16_t &i) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_INT16) ?
            (i = g_variant_get_int16(gv)), true:
            false;
}

GVariant * marshal(const TYPE_T, const uint64_t b) {
    return g_variant_new_uint64(b);
}

bool unmarshal(const TYPE_T, GVariant *gv, uint64_t &b) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_UINT64) ?
            (b = g_variant_get_uint64(gv)), true:
            false;
}

GVariant * marshal(const TYPE_B, const bool b) {
    return g_variant_new_boolean((gboolean)b);
}

bool unmarshal(const TYPE_B, GVariant *gv, bool &b) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_BOOLEAN) ?
            (b = static_cast<bool>(g_variant_get_boolean(gv))), true:
            false;
}

GVariant * marshal(const TYPE_D, const double x) {
    return g_variant_new_double(x);
}

bool unmarshal(const TYPE_D, GVariant *gv, double &x) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_DOUBLE) ?
            (x =  static_cast<bool>(g_variant_get_double(gv))), true:
            false;
}

GVariant * marshal(const TYPE_O, const std::string &path) {
    return g_variant_is_object_path(path.c_str()) ?
        g_variant_new_object_path(path.c_str()) :
        nullptr;
}

bool unmarshal(const TYPE_O, GVariant *gv, std::string &path) {
    return  g_variant_is_of_type(gv, G_VARIANT_TYPE_OBJECT_PATH) ?
                (path.assign(g_variant_get_string(gv, nullptr) ?: ""), true):
                false;
}

GVariant * marshal(const TYPE_V, const std::string &v) {
    return g_variant_new_variant(g_variant_new_string(v.c_str()));
}

bool unmarshal(const TYPE_V, GVariant *gv, std::string &v) {
    v.clear();
    GVariant *body = nullptr;
    gchar * str = nullptr;

    const bool res =
        g_variant_is_of_type(gv, G_VARIANT_TYPE_VARIANT) &&
        (body = g_variant_get_variant(gv)) &&
        (str = g_variant_print(body, false));
    if (res)
        v.assign(str);
    if (body)
        g_variant_unref(body);
    if (str)
        g_free(str);
    return res;
}

GVariant * marshal(const TYPE_AS, const str_arr_t &arr) {
    GVariantBuilder *build = g_variant_builder_new(G_VARIANT_TYPE("as"));
    for (const std::string &s: arr)
        g_variant_builder_add_value(build, g_variant_new_string(s.c_str()) );
    return g_variant_builder_end(build);
}

bool unmarshal(const TYPE_AS, GVariant *g, str_arr_t &arr) {
    arr.clear();
    if ( !g_variant_type_is_array(g_variant_get_type(g)) )
        return false;

    GVariantIter i;
    g_variant_iter_init (&i, g);
    for (gchar *s; g_variant_iter_loop(&i, "s", &s);)
        arr.push_back(s);
    return true;
}

GVariant * marshal(const TYPE_AO, const str_arr_t &arr) {
    GVariantBuilder *build = g_variant_builder_new(G_VARIANT_TYPE("ao"));
    for (const std::string &s: arr)
        g_variant_builder_add_value(build, g_variant_new_object_path(s.c_str()));
    return g_variant_builder_end(build);
}

bool unmarshal(const TYPE_AO, GVariant *g, str_arr_t &arr) {
    arr.clear();
    if ( !g_variant_type_is_array(g_variant_get_type(g)) )
        return false;

    GVariantIter i;
    g_variant_iter_init (&i, g);
    for (gchar *s; g_variant_iter_loop(&i, "o", &s);)
        arr.push_back(s);
    return true;
}

GVariant * marshal(const TYPE_DICT, const std::map<string, string> &items) {
    GVariantBuilder *build = g_variant_builder_new(G_VARIANT_TYPE("a{ss}"));
    for (const auto &e: items)
        g_variant_builder_add(build, "{ss}",
                              g_variant_new_string(e.first.c_str()),
                              g_variant_new_string(e.second.c_str()) );
    return g_variant_builder_end(build);
}

bool unmarshal(const TYPE_DICT, GVariant *g, dict_t &map) {
    map.clear();
    if ( !g_variant_type_is_array(g_variant_get_type(g)) )
        return false;

    GVariantIter i;
    g_variant_iter_init (&i, g);
    for (gchar *k, *v; g_variant_iter_loop(&i, "{ss}", &k, &v);)
        map[k] = v;

    return true;
}

/* GVariant * marshal(const TYPE_VDICT, const std::map<string, string> &items) {// Not implemented: not needed.
    return nullptr;
} */

bool unmarshal(const TYPE_VDICT, GVariant *g, dict_t &map) {
    map.clear();
    if ( !g_variant_type_is_array(g_variant_get_type(g)) )
        return false;

    GVariantIter i;
    GVariant *v;
    g_variant_iter_init (&i, g);
    for (gchar *k; g_variant_iter_loop(&i, "{sv}", &k, &v);) {
        gchar *s = g_variant_print(v, false);
        map[k] = (s ?: "<NULL>");
        g_free(s);
    }
    return true;
}

/* GVariant * marshal(const TYPE_ATUP, const tuple_arr_t &) {       // This one is not implemented;
    return nullptr;                                                 // TYPE_ATUP is intended for output parameters only
} */                                                                //      and does not map to any specific Dbus type.

bool unmarshal(const TYPE_ATUP, GVariant *g, tuple_arr_t &arr) {
    arr.clear();
    if ( !g_variant_type_is_array(g_variant_get_type(g)) ) {
        return false;
    }

    GVariantIter i;
    g_variant_iter_init (&i, g);
    for (GVariant *tuple; (tuple = g_variant_iter_next_value(&i));) {
        if (g_variant_is_container(tuple)) {
            arr.emplace_back();
            GVariantIter j;
            g_variant_iter_init (&j, tuple);
            for (GVariant *field; (field = g_variant_iter_next_value(&j));) {
                arr.back().emplace_back();
                variants.insert(&arr.back().back(), variant_t{field});   //field is to be unreferenced in dtor of this variant_t
            }
        }
        g_variant_unref(tuple);
    }
    return true;
}

/* GVariant * marshal(const TYPE_ANY, const std::string &) {        // This one is not implemented;
    return nullptr;                                                 // TYPE_ANY is intended for output parameters only
} */                                                                //      and does not map to any specific Dbus type.


bool unmarshal(const TYPE_ANY, GVariant *g, std::string &s) {
    gchar *gs = g_variant_print(g, false);
    s.assign(gs? gs : "<DECODING ERROR>");
    g_free(gs);
    return gs != nullptr;
}


}
}
