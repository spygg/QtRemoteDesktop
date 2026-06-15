/****************************************************************************
** Meta object code from reading C++ file 'screencapturer.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.7.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/server/screencapturer.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'screencapturer.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.7.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
struct qt_meta_stringdata_ScreenCapturer_t {
    QByteArrayData data[7];
    char stringdata0[69];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ScreenCapturer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ScreenCapturer_t qt_meta_stringdata_ScreenCapturer = {
    {
QT_MOC_LITERAL(0, 0, 14), // "ScreenCapturer"
QT_MOC_LITERAL(1, 15, 13), // "frameCaptured"
QT_MOC_LITERAL(2, 29, 0), // ""
QT_MOC_LITERAL(3, 30, 5), // "frame"
QT_MOC_LITERAL(4, 36, 12), // "screenLocked"
QT_MOC_LITERAL(5, 49, 6), // "locked"
QT_MOC_LITERAL(6, 56, 12) // "captureFrame"

    },
    "ScreenCapturer\0frameCaptured\0\0frame\0"
    "screenLocked\0locked\0captureFrame"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ScreenCapturer[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
       3,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   29,    2, 0x06 /* Public */,
       4,    1,   32,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       6,    0,   35,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QImage,    3,
    QMetaType::Void, QMetaType::Bool,    5,

 // slots: parameters
    QMetaType::Void,

       0        // eod
};

void ScreenCapturer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        ScreenCapturer *_t = static_cast<ScreenCapturer *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->frameCaptured((*reinterpret_cast< const QImage(*)>(_a[1]))); break;
        case 1: _t->screenLocked((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 2: _t->captureFrame(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        void **func = reinterpret_cast<void **>(_a[1]);
        {
            typedef void (ScreenCapturer::*_t)(const QImage & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&ScreenCapturer::frameCaptured)) {
                *result = 0;
                return;
            }
        }
        {
            typedef void (ScreenCapturer::*_t)(bool );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&ScreenCapturer::screenLocked)) {
                *result = 1;
                return;
            }
        }
    }
}

const QMetaObject ScreenCapturer::staticMetaObject = {
    { &QObject::staticMetaObject, qt_meta_stringdata_ScreenCapturer.data,
      qt_meta_data_ScreenCapturer,  qt_static_metacall, Q_NULLPTR, Q_NULLPTR}
};


const QMetaObject *ScreenCapturer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ScreenCapturer::qt_metacast(const char *_clname)
{
    if (!_clname) return Q_NULLPTR;
    if (!strcmp(_clname, qt_meta_stringdata_ScreenCapturer.stringdata0))
        return static_cast<void*>(const_cast< ScreenCapturer*>(this));
    return QObject::qt_metacast(_clname);
}

int ScreenCapturer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void ScreenCapturer::frameCaptured(const QImage & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ScreenCapturer::screenLocked(bool _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_END_MOC_NAMESPACE
