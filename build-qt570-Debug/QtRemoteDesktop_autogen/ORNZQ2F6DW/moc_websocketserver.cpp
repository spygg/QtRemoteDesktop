/****************************************************************************
** Meta object code from reading C++ file 'websocketserver.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.7.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/server/websocketserver.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'websocketserver.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.7.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
struct qt_meta_stringdata_WebSocketServer_t {
    QByteArrayData data[18];
    char stringdata0[246];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_WebSocketServer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_WebSocketServer_t qt_meta_stringdata_WebSocketServer = {
    {
QT_MOC_LITERAL(0, 0, 15), // "WebSocketServer"
QT_MOC_LITERAL(1, 16, 15), // "clientConnected"
QT_MOC_LITERAL(2, 32, 0), // ""
QT_MOC_LITERAL(3, 33, 8), // "clientId"
QT_MOC_LITERAL(4, 42, 18), // "clientDisconnected"
QT_MOC_LITERAL(5, 61, 13), // "inputReceived"
QT_MOC_LITERAL(6, 75, 4), // "data"
QT_MOC_LITERAL(7, 80, 20), // "codecChangeRequested"
QT_MOC_LITERAL(8, 101, 5), // "codec"
QT_MOC_LITERAL(9, 107, 19), // "modeChangeRequested"
QT_MOC_LITERAL(10, 127, 4), // "mode"
QT_MOC_LITERAL(11, 132, 17), // "fileChunkReceived"
QT_MOC_LITERAL(12, 150, 4), // "path"
QT_MOC_LITERAL(13, 155, 15), // "onNewConnection"
QT_MOC_LITERAL(14, 171, 20), // "onSocketDisconnected"
QT_MOC_LITERAL(15, 192, 21), // "onTextMessageReceived"
QT_MOC_LITERAL(16, 214, 7), // "message"
QT_MOC_LITERAL(17, 222, 23) // "onBinaryMessageReceived"

    },
    "WebSocketServer\0clientConnected\0\0"
    "clientId\0clientDisconnected\0inputReceived\0"
    "data\0codecChangeRequested\0codec\0"
    "modeChangeRequested\0mode\0fileChunkReceived\0"
    "path\0onNewConnection\0onSocketDisconnected\0"
    "onTextMessageReceived\0message\0"
    "onBinaryMessageReceived"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_WebSocketServer[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
      10,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       6,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   64,    2, 0x06 /* Public */,
       4,    1,   67,    2, 0x06 /* Public */,
       5,    2,   70,    2, 0x06 /* Public */,
       7,    1,   75,    2, 0x06 /* Public */,
       9,    1,   78,    2, 0x06 /* Public */,
      11,    2,   81,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      13,    0,   86,    2, 0x08 /* Private */,
      14,    0,   87,    2, 0x08 /* Private */,
      15,    1,   88,    2, 0x08 /* Private */,
      17,    1,   91,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString, QMetaType::QJsonObject,    3,    6,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void, QMetaType::QString,   10,
    QMetaType::Void, QMetaType::QString, QMetaType::QByteArray,   12,    6,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   16,
    QMetaType::Void, QMetaType::QByteArray,   16,

       0        // eod
};

void WebSocketServer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        WebSocketServer *_t = static_cast<WebSocketServer *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->clientConnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->clientDisconnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->inputReceived((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QJsonObject(*)>(_a[2]))); break;
        case 3: _t->codecChangeRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->modeChangeRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->fileChunkReceived((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QByteArray(*)>(_a[2]))); break;
        case 6: _t->onNewConnection(); break;
        case 7: _t->onSocketDisconnected(); break;
        case 8: _t->onTextMessageReceived((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 9: _t->onBinaryMessageReceived((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        void **func = reinterpret_cast<void **>(_a[1]);
        {
            typedef void (WebSocketServer::*_t)(const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::clientConnected)) {
                *result = 0;
                return;
            }
        }
        {
            typedef void (WebSocketServer::*_t)(const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::clientDisconnected)) {
                *result = 1;
                return;
            }
        }
        {
            typedef void (WebSocketServer::*_t)(const QString & , const QJsonObject & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::inputReceived)) {
                *result = 2;
                return;
            }
        }
        {
            typedef void (WebSocketServer::*_t)(const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::codecChangeRequested)) {
                *result = 3;
                return;
            }
        }
        {
            typedef void (WebSocketServer::*_t)(const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::modeChangeRequested)) {
                *result = 4;
                return;
            }
        }
        {
            typedef void (WebSocketServer::*_t)(const QString & , const QByteArray & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&WebSocketServer::fileChunkReceived)) {
                *result = 5;
                return;
            }
        }
    }
}

const QMetaObject WebSocketServer::staticMetaObject = {
    { &QObject::staticMetaObject, qt_meta_stringdata_WebSocketServer.data,
      qt_meta_data_WebSocketServer,  qt_static_metacall, Q_NULLPTR, Q_NULLPTR}
};


const QMetaObject *WebSocketServer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *WebSocketServer::qt_metacast(const char *_clname)
{
    if (!_clname) return Q_NULLPTR;
    if (!strcmp(_clname, qt_meta_stringdata_WebSocketServer.stringdata0))
        return static_cast<void*>(const_cast< WebSocketServer*>(this));
    return QObject::qt_metacast(_clname);
}

int WebSocketServer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 10)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 10;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 10)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 10;
    }
    return _id;
}

// SIGNAL 0
void WebSocketServer::clientConnected(const QString & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void WebSocketServer::clientDisconnected(const QString & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void WebSocketServer::inputReceived(const QString & _t1, const QJsonObject & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void WebSocketServer::codecChangeRequested(const QString & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void WebSocketServer::modeChangeRequested(const QString & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void WebSocketServer::fileChunkReceived(const QString & _t1, const QByteArray & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}
QT_END_MOC_NAMESPACE
