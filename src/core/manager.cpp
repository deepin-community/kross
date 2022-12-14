/***************************************************************************
 * manager.cpp
 * This file is part of the KDE project
 * copyright (C)2004-2007 by Sebastian Sauer (mail@dipe.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 * You should have received a copy of the GNU Library General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 ***************************************************************************/

#include "manager.h"
#include "interpreter.h"
#include "action.h"
#include "actioncollection.h"
#include "kross_debug.h"

#include <QObject>
#include <QArgument>
#include <QFile>
#include <QRegExp>
#include <QFileInfo>
#include <QPointer>
#include <QLibrary>
#include <QCoreApplication>

#include <klocalizedstring.h>

extern "C"
{
    typedef QObject *(*def_module_func)();
}

using namespace Kross;

namespace Kross
{

/// @internal
class Manager::Private
{
public:
    /// List of \a InterpreterInfo instances.
    QHash< QString, InterpreterInfo * > interpreterinfos;

    /// List of the interpreter names.
    QStringList interpreters;

    /// Loaded modules.
    QHash< QString, QPointer<QObject> > modules;

    /// The collection of \a Action instances.
    ActionCollection *collection;

    /// List with custom handlers for metatypes.
    QHash<QByteArray, MetaTypeHandler *> wrappers;

    /// Strict type handling enabled or disabled.
    bool strictTypesEnabled;
};

}

Q_GLOBAL_STATIC(Manager, _self)

Manager &Manager::self()
{
    return *_self();
}

enum LibraryFunction
{
    LibraryFunctionInterpreter,
    LibraryFunctionModule
};

static QFunctionPointer loadLibrary(const char *libname, enum LibraryFunction function)
{
    QLibrary lib;
    QString libAbsoluteFilePath;
    foreach (const QString &path, QCoreApplication::instance()->libraryPaths()) {
        const QFileInfo &fileInfo = QFileInfo(path, libname);
        lib.setFileName(fileInfo.filePath());
        lib.setLoadHints(QLibrary::ExportExternalSymbolsHint);
        if (lib.load()) {
            libAbsoluteFilePath = fileInfo.absoluteFilePath();
            break;
        }
    }

    if (!lib.isLoaded()) {
#ifdef KROSS_INTERPRETER_DEBUG
        if (function == LibraryFunctionInterpreter) {
            qCDebug(KROSS_LOG) << "Kross Interpreter '" << libname <<
                "' not available: " << lib.errorString();
        } else if (function == LibraryFunctionModule) {
            qCDebug(KROSS_LOG) << "Kross Module '" << libname <<
                "' not available: " << lib.errorString();
        } else {
            qCWarning(KROSS_LOG) << "Failed to load unknown type of '" <<
                libname << "' library: " << lib.errorString();
        }
#endif
        return nullptr;
    }

    const char* functionName = function == LibraryFunctionInterpreter ? "krossinterpreter" : "krossmodule";
    QFunctionPointer funcPtr = lib.resolve(functionName);
    if (!funcPtr) {
        qCWarning(KROSS_LOG) << QStringLiteral("Failed to resolve %1 in %2%3")
            .arg(functionName)
            .arg(lib.fileName())
            .arg(libAbsoluteFilePath.isEmpty() ? "" : QString(" (%1)").arg(libAbsoluteFilePath));
    }
    return funcPtr;
}

Manager::Manager()
    : QObject()
    , QScriptable()
    , ChildrenInterface()
    , d(new Private())
{
    d->strictTypesEnabled = true;
    setObjectName("Kross");
    d->collection = new ActionCollection("main");

#ifdef KROSS_PYTHON_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_PYTHON_LIBRARY, LibraryFunctionInterpreter)) {
        d->interpreterinfos.insert("python",
                                   new InterpreterInfo("python",
                                           funcPtr, // library
                                           "*.py", // file filter-wildcard
                                           QStringList() << "text/x-python" // mimetypes
                                                      )
                                  );
    }
#endif

#ifdef KROSS_RUBY_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_RUBY_LIBRARY, LibraryFunctionInterpreter)) {
        InterpreterInfo::Option::Map options;
        options.insert("safelevel", new InterpreterInfo::Option(
                           i18n("Level of safety of the Ruby interpreter"),
                           QVariant(0)));  // 0 -> unsafe, 4 -> very safe
        d->interpreterinfos.insert("ruby",
                                   new InterpreterInfo("ruby",
                                           funcPtr, // library
                                           "*.rb", // file filter-wildcard
                                           QStringList() << /* "text/x-ruby" << */ "application/x-ruby", // mimetypes
                                           options // options
                                                      )
                                  );
    }
#endif

#ifdef KROSS_JAVA_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_JAVA_LIBRARY, LibraryFunctionInterpreter)) {
        d->interpreterinfos.insert("java",
                                   new InterpreterInfo("java",
                                           funcPtr, // library
                                           "*.java *.class *.jar", // file filter-wildcard
                                           QStringList() << "application/java" // mimetypes
                                                      )
                                  );
    }
#endif

#ifdef KROSS_FALCON_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_FALCON_LIBRARY, LibraryFunctionInterpreter)) {
        d->interpreterinfos.insert("falcon",
                                   new InterpreterInfo("falcon",
                                           funcPtr, // library
                                           "*.fal", // file filter-wildcard
                                           QStringList() << "application/x-falcon" // mimetypes
                                                      )
                                  );
    }
#endif

#ifdef KROSS_QTSCRIPT_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_QTSCRIPT_LIBRARY, LibraryFunctionInterpreter)) {
        d->interpreterinfos.insert("qtscript",
                                   new InterpreterInfo("qtscript",
                                           funcPtr, // library
                                           "*.es", // file filter-wildcard
                                           QStringList() << "application/ecmascript" // mimetypes
                                                      )
                                  );
    }
#endif

#ifdef KROSS_LUA_LIBRARY
    if (QFunctionPointer funcPtr = loadLibrary(KROSS_LUA_LIBRARY, LibraryFunctionInterpreter)) {
        d->interpreterinfos.insert("lua",
                                   new InterpreterInfo("lua",
                                           funcPtr, // library
                                           "*.lua *.luac", // file filter-wildcard
                                           QStringList() << "application/x-lua" // mimetypes
                                                      )
                                  );
    }
#endif

    // fill the list of supported interpreternames.
    QHash<QString, InterpreterInfo *>::Iterator it(d->interpreterinfos.begin());
    for (; it != d->interpreterinfos.end(); ++it)
        if (it.value()) {
            d->interpreters << it.key();
        }
    d->interpreters.sort();

    // publish ourself.
    ChildrenInterface::addObject(this, "Kross");
}

Manager::~Manager()
{
    qDeleteAll(d->wrappers);
    qDeleteAll(d->interpreterinfos);
    qDeleteAll(d->modules);
    delete d->collection;
    delete d;
}

QHash< QString, InterpreterInfo * > Manager::interpreterInfos() const
{
    return d->interpreterinfos;
}

bool Manager::hasInterpreterInfo(const QString &interpretername) const
{
    return d->interpreterinfos.contains(interpretername) && d->interpreterinfos[interpretername];
}

InterpreterInfo *Manager::interpreterInfo(const QString &interpretername) const
{
    return hasInterpreterInfo(interpretername) ? d->interpreterinfos[interpretername] : nullptr;
}

const QString Manager::interpreternameForFile(const QString &file)
{
    QRegExp rx;
    rx.setPatternSyntax(QRegExp::Wildcard);
    for (QHash<QString, InterpreterInfo *>::Iterator it = d->interpreterinfos.begin(); it != d->interpreterinfos.end(); ++it) {
        if (! it.value()) {
            continue;
        }
        foreach (const QString &wildcard, it.value()->wildcard().split(' ', Qt::SkipEmptyParts)) {
            rx.setPattern(wildcard);
            if (rx.exactMatch(file)) {
                return it.value()->interpreterName();
            }
        }
    }
    return QString();
}

Interpreter *Manager::interpreter(const QString &interpretername) const
{
    if (! hasInterpreterInfo(interpretername)) {
        qCWarning(KROSS_LOG) << "No such interpreter " << interpretername;
        return nullptr;
    }
    return d->interpreterinfos[interpretername]->interpreter();
}

QStringList Manager::interpreters() const
{
    return d->interpreters;
}

ActionCollection *Manager::actionCollection() const
{
    return d->collection;
}

bool Manager::hasAction(const QString &name)
{
    return findChild< Action * >(name) != nullptr;
}

QObject *Manager::action(const QString &name)
{
    Action *action = findChild< Action * >(name);
    if (! action) {
        action = new Action(this, name);
#if 0
        d->actioncollection->insert(action); //FIXME should we really remember the action?
#endif
    }
    return action;
}

QObject *Manager::module(const QString &modulename)
{
    if (d->modules.contains(modulename)) {
        QObject *obj = d->modules[modulename];
        if (obj) {
            return obj;
        }
    }

    if (modulename.isEmpty() || modulename.contains(QRegExp("[^a-zA-Z0-9]"))) {
        qCWarning(KROSS_LOG) << "Invalid module name " << modulename;
        return nullptr;
    }

    QByteArray libraryname = QString("krossmodule%1").arg(modulename).toLower().toLatin1();

    if (QFunctionPointer funcPtr = loadLibrary(libraryname.constData(), LibraryFunctionModule)) {
        def_module_func func = (def_module_func) funcPtr;
        Q_ASSERT(func);
        QObject *module = (QObject *)(func)(); // call the function
        Q_ASSERT(module);
        //krossdebug( QString("Manager::module Module successfully loaded: modulename=%1 module.objectName=%2 module.className=%3").arg(modulename).arg(module->objectName()).arg(module->metaObject()->className()) );
        d->modules.insert(modulename, module);
        return module;
    } else {
        qCWarning(KROSS_LOG) << "Failed to load module " << modulename;
    }
    return nullptr;
}

void Manager::deleteModules()
{
    qDeleteAll(d->modules);
    d->modules.clear();
}

bool Manager::executeScriptFile(const QUrl &file)
{
    qCDebug(KROSS_LOG) << "Manager::executeScriptFile() file=" << file.toString();
    Action *action = new Action(nullptr /*no parent*/, file);
    action->trigger();
    bool ok = ! action->hadError();
    delete action; //action->delayedDestruct();
    return ok;
}

void Manager::addQObject(QObject *obj, const QString &name)
{
    this->addObject(obj, name);
}

QObject *Manager::qobject(const QString &name) const
{
    return this->object(name);
}

QStringList Manager::qobjectNames() const
{
    return this->objects().keys();
}

MetaTypeHandler *Manager::metaTypeHandler(const QByteArray &typeName) const
{
    return d->wrappers.contains(typeName) ? d->wrappers[typeName] : nullptr;
}

void Manager::registerMetaTypeHandler(const QByteArray &typeName, MetaTypeHandler::FunctionPtr *handler)
{
    d->wrappers.insert(typeName, new MetaTypeHandler(handler));
}

void Manager::registerMetaTypeHandler(const QByteArray &typeName, MetaTypeHandler::FunctionPtr2 *handler)
{
    d->wrappers.insert(typeName, new MetaTypeHandler(handler));
}

void Manager::registerMetaTypeHandler(const QByteArray &typeName, MetaTypeHandler *handler)
{
    d->wrappers.insert(typeName, handler);
}

bool Manager::strictTypesEnabled() const
{
    return d->strictTypesEnabled;
}

void Manager::setStrictTypesEnabled(bool enabled)
{
    d->strictTypesEnabled = enabled;
}

bool Manager::hasHandlerAssigned(const QByteArray &typeName) const
{
    return d->wrappers.contains(typeName);
}

