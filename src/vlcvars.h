#pragma once

#include <QList>
#include <QString>
#include <QVariant>

// Thin bridge to libvlccore's object variables.
//
// The public libVLC 3 API only exposes the equalizer, but VLC's other audio and
// video filters (compressor, reverb, sepia, ...) are driven by object variables.
// libvlc_media_player_t is a vlc_object_t in VLC 3, and the audio/video outputs
// are objects beneath it, so we can set those variables directly.
namespace VlcVars {

enum class Type { Bool, Int, Float, String };

// Objects of the given type ("audio output", "video output", "input") found
// beneath root. Each returned pointer holds a reference; call release().
QList<void *> findObjects(void *root, const char *type);
void release(const QList<void *> &objects);

// Creates the variable if missing, then sets it. Returns false on type mismatch.
bool setBool(void *obj, const char *name, bool value);
bool setInt(void *obj, const char *name, qint64 value);
bool setFloat(void *obj, const char *name, float value);
bool setString(void *obj, const char *name, const QString &value);

// Sets the variable only if it already exists on obj.
bool setIfExists(void *obj, const char *name, Type type, const QVariant &value);

QString getString(void *obj, const char *name);
// Sets an existing coordinates variable (e.g. the vout's "mouse-clicked").
bool setCoords(void *obj, const char *name, int x, int y);

// Debug helper: prints the object tree under root.
void dumpTree(void *root, int depth = 0);

// Every plugin module VLC has loaded (needs a libVLC instance alive).
struct Module {
    QString name, longName, capability, object;
    int score;
};
QList<Module> modules();

} // namespace VlcVars
