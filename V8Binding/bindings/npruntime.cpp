/*
 * Copyright (C) 2004, 2006 Apple Computer, Inc.  All rights reserved.
 * Copyright (C) 2007-2009 Google, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "NPV8Object.h"
#include "npruntime_priv.h"
#include "V8NPObject.h"

#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Assertions.h>

// FIXME: Consider removing locks if we're singlethreaded already.
// The static initializer here should work okay, but we want to avoid
// static initialization in general.
//
// Commenting out the locks to avoid dependencies on chrome for now.
// Need a platform abstraction which we can use.
// static Lock StringIdentifierMapLock;

namespace {

// We use StringKey here as the key-type to avoid a string copy to
// construct the map key and for faster comparisons than strcmp.
class StringKey {
  public:
    explicit StringKey(const char* str)
        : m_string(str), m_length(strlen(str)) {}
    StringKey() : m_string(0), m_length(0) {}
    explicit StringKey(WTF::HashTableDeletedValueType)
        : m_string(hashTableDeletedValue()), m_length(0) { }

    StringKey& operator=(const StringKey& other) {
        this->m_string = other.m_string;
        this->m_length = other.m_length;
        return *this;
    }

    bool isHashTableDeletedValue() const {
        return m_string == hashTableDeletedValue();
    }

    const char* m_string;
    size_t m_length;
  private:
    const char* hashTableDeletedValue() const {
        return reinterpret_cast<const char*>(-1);
    }
};

inline bool operator==(const StringKey& x, const StringKey& y) {
    if (x.m_length != y.m_length) {
        return false;
    } else if (x.m_string == y.m_string) {
        return true;
    } else {
        ASSERT(!x.isHashTableDeletedValue() && !y.isHashTableDeletedValue());
        return memcmp(x.m_string, y.m_string, y.m_length) == 0;
    }
}

// Implement WTF::DefaultHash<StringKey>::Hash interface.
struct StringKeyHash {
    static unsigned hash(const StringKey& key) {
        // Compute string hash.
        unsigned hash = 0;
        size_t len = key.m_length;
        const char* str = key.m_string;
        for (size_t i = 0; i < len; i++) {
            char c = str[i];
            hash += c;
            hash += (hash << 10);
            hash ^= (hash >> 6);
        }
        hash += (hash << 3);
        hash ^= (hash >> 11);
        hash += (hash << 15);
        if (hash == 0) {
            hash = 27;
        }
        return hash;
    }

    static bool equal(const StringKey& x, const StringKey& y) {
        return x == y;
    }

    static const bool safeToCompareToEmptyOrDeleted = true;
};

}  // namespace

// Implement HashTraits<StringKey>
struct StringKeyHashTraits : WTF::GenericHashTraits<StringKey> {
    static void constructDeletedValue(StringKey& slot) {
        new (&slot) StringKey(WTF::HashTableDeletedValue);
    }
    static bool isDeletedValue(const StringKey& value) {
        return value.isHashTableDeletedValue();
    }
};

typedef WTF::HashMap<StringKey, PrivateIdentifier*, \
        StringKeyHash, StringKeyHashTraits> StringIdentifierMap;

static StringIdentifierMap* getStringIdentifierMap() {
    static StringIdentifierMap* stringIdentifierMap = 0;
    if (!stringIdentifierMap)
        stringIdentifierMap = new StringIdentifierMap();
    return stringIdentifierMap;
}

// FIXME: Consider removing locks if we're singlethreaded already.
// static Lock IntIdentifierMapLock;

typedef WTF::HashMap<int, PrivateIdentifier*> IntIdentifierMap;

static IntIdentifierMap* getIntIdentifierMap() {
    static IntIdentifierMap* intIdentifierMap = 0;
    if (!intIdentifierMap)
        intIdentifierMap = new IntIdentifierMap();
    return intIdentifierMap;
}

extern "C" {

NPIdentifier NPN_GetStringIdentifier(const NPUTF8* name) {
    ASSERT(name);

    if (name) {
        // AutoLock safeLock(StringIdentifierMapLock);

        StringKey key(name);
        StringIdentifierMap* identMap = getStringIdentifierMap();
        StringIdentifierMap::iterator iter = identMap->find(key);
        if (iter != identMap->end())
            return static_cast<NPIdentifier>(iter->second);

        size_t nameLen = key.m_length;

        // We never release identifiers, so this dictionary will grow.
        PrivateIdentifier* identifier = static_cast<PrivateIdentifier*>(
            malloc(sizeof(PrivateIdentifier) + nameLen + 1));
        char* nameStorage = reinterpret_cast<char*>(identifier + 1);
        memcpy(nameStorage, name, nameLen + 1);
        identifier->isString = true;
        identifier->value.string = reinterpret_cast<NPUTF8*>(nameStorage);
        key.m_string = nameStorage;
        identMap->set(key, identifier);
        return (NPIdentifier)identifier;
    }

    return 0;
}

void NPN_GetStringIdentifiers(const NPUTF8** names, int32_t nameCount,
                              NPIdentifier* identifiers) {
    ASSERT(names);
    ASSERT(identifiers);

    if (names && identifiers)
        for (int i = 0; i < nameCount; i++)
            identifiers[i] = NPN_GetStringIdentifier(names[i]);
}

NPIdentifier NPN_GetIntIdentifier(int32_t intid) {
    // AutoLock safeLock(IntIdentifierMapLock);
    // Special case for -1 and 0, both cannot be used as key in HashMap.
    if (intid == 0 || intid == -1) {
        static PrivateIdentifier* minusOneOrZeroIds[2];
        PrivateIdentifier* id = minusOneOrZeroIds[intid + 1];
        if (!id) {
            id = reinterpret_cast<PrivateIdentifier*>(
                malloc(sizeof(PrivateIdentifier)));
            id->isString = false;
            id->value.number = intid;
            minusOneOrZeroIds[intid + 1] = id;
        }
        return (NPIdentifier)id;
    }

    IntIdentifierMap* identMap = getIntIdentifierMap();
    IntIdentifierMap::iterator iter = identMap->find(intid);
    if (iter != identMap->end())
        return static_cast<NPIdentifier>(iter->second);

    // We never release identifiers, so this dictionary will grow.
    PrivateIdentifier* identifier = reinterpret_cast<PrivateIdentifier*>(
        malloc(sizeof(PrivateIdentifier)));
    identifier->isString = false;
    identifier->value.number = intid;
    identMap->set(intid, identifier);
    return (NPIdentifier)identifier;
}

bool NPN_IdentifierIsString(NPIdentifier identifier) {
    PrivateIdentifier* i = reinterpret_cast<PrivateIdentifier*>(identifier);
    return i->isString;
}

NPUTF8 *NPN_UTF8FromIdentifier(NPIdentifier identifier) {
    PrivateIdentifier* i = reinterpret_cast<PrivateIdentifier*>(identifier);
    if (!i->isString || !i->value.string)
        return NULL;

    return (NPUTF8 *)strdup(i->value.string);
}

int32_t NPN_IntFromIdentifier(NPIdentifier identifier) {
    PrivateIdentifier* i = reinterpret_cast<PrivateIdentifier*>(identifier);
    if (i->isString)
        return 0;
    return i->value.number;
}

void NPN_ReleaseVariantValue(NPVariant* variant) {
    ASSERT(variant);

    if (variant->type == NPVariantType_Object) {
        NPN_ReleaseObject(variant->value.objectValue);
        variant->value.objectValue = 0;
    } else if (variant->type == NPVariantType_String) {
        free((void*)variant->value.stringValue.UTF8Characters);
        variant->value.stringValue.UTF8Characters = 0;
        variant->value.stringValue.UTF8Length = 0;
    }

    variant->type = NPVariantType_Void;
}

NPObject *NPN_CreateObject(NPP npp, NPClass* aClass) {
    ASSERT(aClass);

    if (aClass) {
        NPObject* obj;
        if (aClass->allocate != NULL)
            obj = aClass->allocate(npp, aClass);
        else
            obj = reinterpret_cast<NPObject*>(malloc(sizeof(NPObject)));

        obj->_class = aClass;
        obj->referenceCount = 1;
        return obj;
    }

    return 0;
}

NPObject* NPN_RetainObject(NPObject* obj) {
    ASSERT(obj);
    ASSERT(obj->referenceCount > 0);

    if (obj)
        obj->referenceCount++;

    return obj;
}

// _NPN_DeallocateObject actually deletes the object.  Technically,
// callers should use NPN_ReleaseObject.  Webkit exposes this function
// to kill objects which plugins may not have properly released.
void _NPN_DeallocateObject(NPObject *obj) {
    ASSERT(obj);
    ASSERT(obj->referenceCount >= 0);

    if (obj) {
        // NPObjects that remain in pure C++ may never have wrappers.
        // Hence, if it's not already alive, don't unregister it.
        // If it is alive, unregister it as the *last* thing we do
        // so that it can do as much cleanup as possible on its own.
        if (_NPN_IsAlive(obj))
            _NPN_UnregisterObject(obj);

        obj->referenceCount = -1;
        if (obj->_class->deallocate)
            obj->_class->deallocate(obj);
        else
            free(obj);
    }
}

void NPN_ReleaseObject(NPObject* obj) {
    ASSERT(obj);
    ASSERT(obj->referenceCount >= 1);

    if (obj && obj->referenceCount >= 1) {
        if (--obj->referenceCount == 0)
            _NPN_DeallocateObject(obj);
    }
}

void _NPN_InitializeVariantWithStringCopy(NPVariant* variant,
                                          const NPString* value) {
    variant->type = NPVariantType_String;
    variant->value.stringValue.UTF8Length = value->UTF8Length;
    variant->value.stringValue.UTF8Characters =
        reinterpret_cast<NPUTF8*>(malloc(sizeof(NPUTF8) * value->UTF8Length));
    memcpy((void*)variant->value.stringValue.UTF8Characters,
           value->UTF8Characters,
           sizeof(NPUTF8) * value->UTF8Length);
}


// NPN_Registry
//
// The registry is designed for quick lookup of NPObjects.
// JS needs to be able to quickly lookup a given NPObject to determine
// if it is alive or not.
// The browser needs to be able to quickly lookup all NPObjects which are
// "owned" by an object.
//
// The g_live_objects is a hash table of all live objects to their owner
// objects.  Presence in this table is used primarily to determine if
// objects are live or not.
//
// The g_root_objects is a hash table of root objects to a set of
// objects that should be deactivated in sync with the root.  A
// root is defined as a top-level owner object.  This is used on
// Frame teardown to deactivate all objects associated
// with a particular plugin.

typedef WTF::HashSet<NPObject*> NPObjectSet;
typedef WTF::HashMap<NPObject*, NPObject*> NPObjectMap;
typedef WTF::HashMap<NPObject*, NPObjectSet*> NPRootObjectMap;

// A map of live NPObjects with pointers to their Roots.
NPObjectMap g_live_objects;

// A map of the root objects and the list of NPObjects
// associated with that object.
NPRootObjectMap g_root_objects;

void _NPN_RegisterObject(NPObject* obj, NPObject* owner) {
    ASSERT(obj);

    // Check if already registered.
    if (g_live_objects.find(obj) != g_live_objects.end()) {
      return;
    }

    if (!owner) {
      // Registering a new owner object.
      ASSERT(g_root_objects.find(obj) == g_root_objects.end());
      g_root_objects.set(obj, new NPObjectSet());
    } else {
      // Always associate this object with it's top-most parent.
      // Since we always flatten, we only have to look up one level.
      NPObjectMap::iterator owner_entry = g_live_objects.find(owner);
      NPObject* parent = NULL;
      if (g_live_objects.end() != owner_entry)
          parent = owner_entry->second;

      if (parent) {
          owner = parent;
      }
      ASSERT(g_root_objects.find(obj) == g_root_objects.end());
      if (g_root_objects.find(owner) != g_root_objects.end())
          g_root_objects.get(owner)->add(obj);
    }

    ASSERT(g_live_objects.find(obj) == g_live_objects.end());
    g_live_objects.set(obj, owner);
}

void _NPN_UnregisterObject(NPObject* obj) {
    ASSERT(obj);
    ASSERT(g_live_objects.find(obj) != g_live_objects.end());

    NPObject* owner = NULL;
    if (g_live_objects.find(obj) != g_live_objects.end())
      owner = g_live_objects.find(obj)->second;

    if (owner == NULL) {
        // Unregistering a owner object; also unregister it's descendants.
        ASSERT(g_root_objects.find(obj) != g_root_objects.end());
        NPObjectSet* set = g_root_objects.get(obj);
        while (set->size() > 0) {
#ifndef NDEBUG
            int size = set->size();
#endif
            NPObject* sub_object = *(set->begin());
            // The sub-object should not be a owner!
            ASSERT(g_root_objects.find(sub_object) == g_root_objects.end());

            // First, unregister the object.
            set->remove(sub_object);
            g_live_objects.remove(sub_object);

            // Remove the JS references to the object.
            ForgetV8ObjectForNPObject(sub_object);

            ASSERT(set->size() < size);
        }
        delete set;
        g_root_objects.remove(obj);
    } else {
        NPRootObjectMap::iterator owner_entry = g_root_objects.find(owner);
        if (owner_entry != g_root_objects.end()) {
          NPObjectSet* list = owner_entry->second;
          ASSERT(list->find(obj) != list->end());
          list->remove(obj);
        }
    }
    g_live_objects.remove(obj);
    ForgetV8ObjectForNPObject(obj);
}

bool _NPN_IsAlive(NPObject* obj) {
    return g_live_objects.find(obj) != g_live_objects.end();
}

}  // extern "C"