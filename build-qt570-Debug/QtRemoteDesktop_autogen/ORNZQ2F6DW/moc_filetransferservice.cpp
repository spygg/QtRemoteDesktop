/****************************************************************************
** Meta object code from reading C++ file 'filetransferservice.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.7.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/server/filetransferservice.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'filetransferservice.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.7.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
struct qt_meta_stringdata_FileTransferService_t {
    QByteArrayData data[21];
    char stringdata0[246];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FileTransferService_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FileTransferService_t qt_meta_stringdata_FileTransferService = {
    {
QT_MOC_LITERAL(0, 0, 19), // "FileTransferService"
QT_MOC_LITERAL(1, 20, 12), // "jsonResponse"
QT_MOC_LITERAL(2, 33, 0), // ""
QT_MOC_LITERAL(3, 34, 8), // "clientId"
QT_MOC_LITERAL(4, 43, 3), // "obj"
QT_MOC_LITERAL(5, 47, 14), // "binaryResponse"
QT_MOC_LITERAL(6, 62, 4), // "data"
QT_MOC_LITERAL(7, 67, 18), // "downloadChunkReady"
QT_MOC_LITERAL(8, 86, 4), // "path"
QT_MOC_LITERAL(9, 91, 6), // "offset"
QT_MOC_LITERAL(10, 98, 9), // "totalSize"
QT_MOC_LITERAL(11, 108, 16), // "transferProgress"
QT_MOC_LITERAL(12, 125, 11), // "transferred"
QT_MOC_LITERAL(13, 137, 5), // "total"
QT_MOC_LITERAL(14, 143, 9), // "speedKBps"
QT_MOC_LITERAL(15, 153, 15), // "processFileList"
QT_MOC_LITERAL(16, 169, 15), // "processDownload"
QT_MOC_LITERAL(17, 185, 18), // "processUploadStart"
QT_MOC_LITERAL(18, 204, 4), // "size"
QT_MOC_LITERAL(19, 209, 18), // "processUploadChunk"
QT_MOC_LITERAL(20, 228, 17) // "processUploadDone"

    },
    "FileTransferService\0jsonResponse\0\0"
    "clientId\0obj\0binaryResponse\0data\0"
    "downloadChunkReady\0path\0offset\0totalSize\0"
    "transferProgress\0transferred\0total\0"
    "speedKBps\0processFileList\0processDownload\0"
    "processUploadStart\0size\0processUploadChunk\0"
    "processUploadDone"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FileTransferService[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   59,    2, 0x06 /* Public */,
       5,    2,   64,    2, 0x06 /* Public */,
       7,    5,   69,    2, 0x06 /* Public */,
      11,    5,   80,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      15,    2,   91,    2, 0x0a /* Public */,
      16,    2,   96,    2, 0x0a /* Public */,
      17,    3,  101,    2, 0x0a /* Public */,
      19,    2,  108,    2, 0x0a /* Public */,
      20,    2,  113,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QJsonObject,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::QByteArray,    3,    6,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::LongLong, QMetaType::QByteArray, QMetaType::LongLong,    3,    8,    9,    6,   10,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::LongLong, QMetaType::LongLong, QMetaType::Double,    3,    8,   12,   13,   14,

 // slots: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    8,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    8,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::LongLong,    3,    8,   18,
    QMetaType::Void, QMetaType::QString, QMetaType::QByteArray,    8,    6,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,    3,    8,

       0        // eod
};

void FileTransferService::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        FileTransferService *_t = static_cast<FileTransferService *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->jsonResponse((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QJsonObject(*)>(_a[2]))); break;
        case 1: _t->binaryResponse((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QByteArray(*)>(_a[2]))); break;
        case 2: _t->downloadChunkReady((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< const QByteArray(*)>(_a[4])),(*reinterpret_cast< qint64(*)>(_a[5]))); break;
        case 3: _t->transferProgress((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< qint64(*)>(_a[4])),(*reinterpret_cast< double(*)>(_a[5]))); break;
        case 4: _t->processFileList((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 5: _t->processDownload((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 6: _t->processUploadStart((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3]))); break;
        case 7: _t->processUploadChunk((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QByteArray(*)>(_a[2]))); break;
        case 8: _t->processUploadDone((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        void **func = reinterpret_cast<void **>(_a[1]);
        {
            typedef void (FileTransferService::*_t)(const QString & , const QJsonObject & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&FileTransferService::jsonResponse)) {
                *result = 0;
                return;
            }
        }
        {
            typedef void (FileTransferService::*_t)(const QString & , const QByteArray & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&FileTransferService::binaryResponse)) {
                *result = 1;
                return;
            }
        }
        {
            typedef void (FileTransferService::*_t)(const QString & , const QString & , qint64 , const QByteArray & , qint64 );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&FileTransferService::downloadChunkReady)) {
                *result = 2;
                return;
            }
        }
        {
            typedef void (FileTransferService::*_t)(const QString & , const QString & , qint64 , qint64 , double );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&FileTransferService::transferProgress)) {
                *result = 3;
                return;
            }
        }
    }
}

const QMetaObject FileTransferService::staticMetaObject = {
    { &QObject::staticMetaObject, qt_meta_stringdata_FileTransferService.data,
      qt_meta_data_FileTransferService,  qt_static_metacall, Q_NULLPTR, Q_NULLPTR}
};


const QMetaObject *FileTransferService::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FileTransferService::qt_metacast(const char *_clname)
{
    if (!_clname) return Q_NULLPTR;
    if (!strcmp(_clname, qt_meta_stringdata_FileTransferService.stringdata0))
        return static_cast<void*>(const_cast< FileTransferService*>(this));
    return QObject::qt_metacast(_clname);
}

int FileTransferService::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void FileTransferService::jsonResponse(const QString & _t1, const QJsonObject & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void FileTransferService::binaryResponse(const QString & _t1, const QByteArray & _t2)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void FileTransferService::downloadChunkReady(const QString & _t1, const QString & _t2, qint64 _t3, const QByteArray & _t4, qint64 _t5)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void FileTransferService::transferProgress(const QString & _t1, const QString & _t2, qint64 _t3, qint64 _t4, double _t5)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)), const_cast<void*>(reinterpret_cast<const void*>(&_t2)), const_cast<void*>(reinterpret_cast<const void*>(&_t3)), const_cast<void*>(reinterpret_cast<const void*>(&_t4)), const_cast<void*>(reinterpret_cast<const void*>(&_t5)) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_END_MOC_NAMESPACE
