/****************************************************************************
** Meta object code from reading C++ file 'rdpserver.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.7.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/server/rdpserver.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'rdpserver.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.7.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
struct qt_meta_stringdata_RDPServer_t {
    QByteArrayData data[26];
    char stringdata0[335];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_RDPServer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_RDPServer_t qt_meta_stringdata_RDPServer = {
    {
QT_MOC_LITERAL(0, 0, 9), // "RDPServer"
QT_MOC_LITERAL(1, 10, 15), // "requestFileList"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 8), // "clientId"
QT_MOC_LITERAL(4, 36, 4), // "path"
QT_MOC_LITERAL(5, 41, 15), // "requestDownload"
QT_MOC_LITERAL(6, 57, 18), // "requestUploadStart"
QT_MOC_LITERAL(7, 76, 4), // "size"
QT_MOC_LITERAL(8, 81, 17), // "requestUploadDone"
QT_MOC_LITERAL(9, 99, 17), // "onClientConnected"
QT_MOC_LITERAL(10, 117, 20), // "onClientDisconnected"
QT_MOC_LITERAL(11, 138, 15), // "onInputReceived"
QT_MOC_LITERAL(12, 154, 5), // "input"
QT_MOC_LITERAL(13, 160, 15), // "onFrameCaptured"
QT_MOC_LITERAL(14, 176, 5), // "frame"
QT_MOC_LITERAL(15, 182, 14), // "onEncodedFrame"
QT_MOC_LITERAL(16, 197, 4), // "data"
QT_MOC_LITERAL(17, 202, 10), // "isKeyframe"
QT_MOC_LITERAL(18, 213, 9), // "timestamp"
QT_MOC_LITERAL(19, 223, 19), // "onHttpNewConnection"
QT_MOC_LITERAL(20, 243, 13), // "onHttpRequest"
QT_MOC_LITERAL(21, 257, 20), // "onCodecConfigChanged"
QT_MOC_LITERAL(22, 278, 9), // "extradata"
QT_MOC_LITERAL(23, 288, 19), // "onFrameForImageMode"
QT_MOC_LITERAL(24, 308, 21), // "onModeChangeRequested"
QT_MOC_LITERAL(25, 330, 4) // "mode"

    },
    "RDPServer\0requestFileList\0\0clientId\0"
    "path\0requestDownload\0requestUploadStart\0"
    "size\0requestUploadDone\0onClientConnected\0"
    "onClientDisconnected\0onInputReceived\0"
    "input\0onFrameCaptured\0frame\0onEncodedFrame\0"
    "data\0isKeyframe\0timestamp\0onHttpNewConnection\0"
    "onHttpRequest\0onCodecConfigChanged\0"
    "extradata\0onFrameForImageMode\0"
    "onModeChangeRequested\0mode"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_RDPServer[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
      14,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   84,    2, 0x06 /* Public */,
       5,    2,   89,    2, 0x06 /* Public */,
       6,    3,   94,    2, 0x06 /* Public */,
       8,    2,  101,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       9,    1,  106,    2, 0x08 /* Private */,
      10,    1,  109,    2, 0x08 /* Private */,
      11,    2,  112,    2, 0x08 /* Private */,
      13,    1,  117,    2, 0x08 /* Private */,
      15,    3,  120,    2, 0x08 /* Private */,
      19,    0,  127,    2, 0x08 /* Private */,
      20,    0,  128,    2, 0x08 /* Private */,
      21,    1,  129,    2, 0x08 /* Private */,
      23,    1,  132,    2, 0x08 /* Private */,
      24,    1,  135,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::LongLong,    3,    4,    7,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    4,

 // slots: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString, QMetaType::QJsonObject,    3,   12,
    QMetaType::Void, QMetaType::QImage,   14,
    QMetaType::Void, QMetaType::QByteArray, QMetaType::Bool, QMetaType::LongLong,   16,   17,   18,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QByteArray,   22,
    QMetaType::Void, QMetaType::QImage,   14,
    QMetaType::Void, QMetaType::QString,   25,

       0        // eod
};

void RDPServer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        RDPServer *_t = static_cast<RDPServer *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->requestFileList((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 1: _t->requestDownload((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 2: _t->requestUploadStart((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3]))); break;
        case 3: _t->requestUploadDone((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 4: _t->onClientConnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->onClientDisconnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 6: _t->onInputReceived((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QJsonObject(*)>(_a[2]))); break;
        case 7: _t->onFrameCaptured((*reinterpret_cast< const QImage(*)>(_a[1]))); break;
        case 8: _t->onEncodedFrame((*reinterpret_cast< const QByteArray(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3]))); break;
        case 9: _t->onHttpNewConnection(); break;
        case 10: _t->onHttpRequest(); break;
        case 11: _t->onCodecConfigChanged((*reinterpret_cast< const QByteArray(*)>(_a[1]))); break;
        case 12: _t->onFrameForImageMode((*reinterpret_cast< const QImage(*)>(_a[1]))); break;
        case 13: _t->onModeChangeRequested((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        void **func = reinterpret_cast<void **>(_a[1]);
        {
            typedef void (RDPServer::*_t)(const QString & , const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&RDPServer::requestFileList)) {
                *result = 0;
                return;
            }
        }
        {
            typedef void (RDPServer::*_t)(const QString & , const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&RDPServer::requestDownload)) {
                *result = 1;
                return;
            }
        }
        {
            typedef void (RDPServer::*_t)(const QString & , const QString & , qint64 );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&RDPServer::requestUploadStart)) {
                *result = 2;
                return;
            }
        }
        {
            typedef void (RDPServer::*_t)(const QString & , const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&RDPServer::requestUploadDone)) {
                *result = 3;
                return;
            }
        }
    }
}

const QMetaObject RDPServer::staticMetaObject = {
    { &QObject::staticMetaObject, qt_meta_stringdata_RDPServer.data,
      qt_meta_data_RDPServer,  qt_static_metacall, Q_NULLPTR, Q_NULLPTR}
};


const QMetaObject *RDPServer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *RDPServer::qt_metacast(const char *_clname)
{
    if (!_clname) return Q_NULLPTR;
    if (!strcmp(_clname, qt_meta_stringdata_RDPServer.stringdata0))
        return static_cast<void*>(const_cast< RDPServer*>(this));
    return QObject::qt_metacast(_clname);
}

int RDPServer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 14;
    }
    return _id;
}

// SIGNAL 0
void RDPServer::requestFileList(const QString & _t1, const QString & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void RDPServer::requestDownload(const QString & _t1, const QString & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void RDPServer::requestUploadStart(const QString & _t1, const QString & _t2, qint64 _t3)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void RDPServer::requestUploadDone(const QString & _t1, const QString & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_END_MOC_NAMESPACE
