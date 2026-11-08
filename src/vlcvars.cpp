#include "vlcvars.h"

#include <QByteArray>
#include <QDebug>
#include <QVariant>

#ifndef MODULE_STRING
#define MODULE_STRING "vlcredesign"
#endif
#include <vlc_common.h>
#include <vlc_variables.h>
#include <vlc_modules.h>

namespace VlcVars {

static vlc_object_t *obj(void *p) { return static_cast<vlc_object_t *>(p); }

static void collect(vlc_object_t *root, const char *type, QList<void *> &out, int depth)
{
    if (depth > 5)
        return;
    vlc_list_t *list = vlc_list_children(root);
    if (!list)
        return;
    for (int i = 0; i < list->i_count; ++i) {
        auto *child = static_cast<vlc_object_t *>(list->p_values[i].p_address);
        if (child->obj.object_type && qstrcmp(child->obj.object_type, type) == 0) {
            vlc_object_hold(child);
            out.append(child);
        }
        collect(child, type, out, depth + 1);
    }
    vlc_list_release(list);
}

QList<void *> findObjects(void *root, const char *type)
{
    QList<void *> out;
    if (root)
        collect(obj(root), type, out, 0);
    return out;
}

void release(const QList<void *> &objects)
{
    for (void *o : objects)
        vlc_object_release(obj(o));
}

// Ensures a variable of the given class exists. Returns false on mismatch.
static bool ensure(vlc_object_t *o, const char *name, int type)
{
    const int existing = var_Type(o, name);
    if (existing == 0)
        return var_Create(o, name, type) == VLC_SUCCESS;
    return (existing & VLC_VAR_CLASS) == type;
}

bool setBool(void *o, const char *name, bool value)
{
    if (!o || !ensure(obj(o), name, VLC_VAR_BOOL))
        return false;
    return var_SetBool(obj(o), name, value) == VLC_SUCCESS;
}

bool setInt(void *o, const char *name, qint64 value)
{
    if (!o || !ensure(obj(o), name, VLC_VAR_INTEGER))
        return false;
    return var_SetInteger(obj(o), name, value) == VLC_SUCCESS;
}

bool setFloat(void *o, const char *name, float value)
{
    if (!o || !ensure(obj(o), name, VLC_VAR_FLOAT))
        return false;
    return var_SetFloat(obj(o), name, value) == VLC_SUCCESS;
}

bool setCoords(void *o, const char *name, int x, int y)
{
    if (!o || (var_Type(obj(o), name) & VLC_VAR_CLASS) != VLC_VAR_COORDS)
        return false;
    return var_SetCoords(obj(o), name, x, y) == VLC_SUCCESS;
}

bool setString(void *o, const char *name, const QString &value)
{
    if (!o || !ensure(obj(o), name, VLC_VAR_STRING))
        return false;
    const QByteArray utf8 = value.toUtf8();
    return var_SetString(obj(o), name, utf8.constData()) == VLC_SUCCESS;
}

bool setIfExists(void *o, const char *name, Type type, const QVariant &value)
{
    if (!o || var_Type(obj(o), name) == 0)
        return false;
    switch (type) {
    case Type::Bool:
        return setBool(o, name, value.toBool());
    case Type::Int:
        return setInt(o, name, value.toLongLong());
    case Type::Float:
        return setFloat(o, name, value.toFloat());
    case Type::String:
        return setString(o, name, value.toString());
    }
    return false;
}

QString getString(void *o, const char *name)
{
    if (!o || (var_Type(obj(o), name) & VLC_VAR_CLASS) != VLC_VAR_STRING)
        return {};
    char *s = var_GetString(obj(o), name);
    const QString out = QString::fromUtf8(s ? s : "");
    free(s);
    return out;
}

void dumpTree(void *root, int depth)
{
    if (!root)
        return;
    vlc_list_t *list = vlc_list_children(obj(root));
    if (!list)
        return;
    for (int i = 0; i < list->i_count; ++i) {
        auto *child = static_cast<vlc_object_t *>(list->p_values[i].p_address);
        qDebug().noquote() << QString(depth * 2, ' ') + QString::fromUtf8(child->obj.object_type);
        dumpTree(child, depth + 1);
    }
    vlc_list_release(list);
}

QList<Module> modules()
{
    QList<Module> out;
    size_t n = 0;
    module_t **list = module_list_get(&n);
    for (size_t i = 0; i < n; ++i) {
        const module_t *m = list[i];
        const char *cap = module_get_capability(m);
        if (!cap || !*cap || !strcmp(cap, "none"))
            continue; // the core itself and sub-module stubs
        out.append({QString::fromUtf8(module_get_name(m, false)), QString::fromUtf8(module_get_name(m, true)),
                    QString::fromUtf8(cap), QString::fromUtf8(module_get_object(m)), module_get_score(m)});
    }
    module_list_free(list);
    return out;
}

} // namespace VlcVars
