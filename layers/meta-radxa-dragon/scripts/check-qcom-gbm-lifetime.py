#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run on the Q6A with its deployed GBM backend; no modesetting required."""
import ctypes as c
import os

lib = c.CDLL('libgbm.so.1')
p, i, u = c.c_void_p, c.c_int, c.c_uint32
class Import(c.Structure):
    _fields_ = [('fd', i), ('width', u), ('height', u), ('stride', u), ('format', u)]
for name, result, args in [
    ('gbm_create_device', p, [i]), ('gbm_device_destroy', None, [p]),
    ('gbm_bo_create', p, [p, u, u, u, u]), ('gbm_bo_destroy', None, [p]),
    ('gbm_bo_get_fd', i, [p]), ('gbm_bo_get_stride', u, [p]),
    ('gbm_bo_import', p, [p, u, p, u]),
]:
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, args

def export(bo):
    fd = lib.gbm_bo_get_fd(bo)
    assert fd >= 0, 'live BO lost its GEM handle'
    os.close(fd)

fmt = 0x34325258
for separate_fd in (False, True):
    fd1 = os.open('/dev/dri/renderD128', os.O_RDWR)
    fd2 = os.open('/dev/dri/renderD128', os.O_RDWR) if separate_fd else fd1
    a = lib.gbm_create_device(fd1)
    bo = lib.gbm_bo_create(a, 64, 64, fmt, 5)
    assert a and bo
    export(bo)
    empty = lib.gbm_create_device(fd2)
    lib.gbm_device_destroy(empty)
    export(bo)
    b = lib.gbm_create_device(fd2)
    other = lib.gbm_bo_create(b, 64, 64, fmt, 5)
    assert other
    lib.gbm_bo_destroy(other)
    export(bo)
    dma = lib.gbm_bo_get_fd(bo)
    data = Import(dma, 64, 64, lib.gbm_bo_get_stride(bo), fmt)
    imported = lib.gbm_bo_import(b, 0x5503, c.byref(data), 5)
    assert imported
    os.close(dma)
    lib.gbm_bo_destroy(imported)
    lib.gbm_device_destroy(b)
    export(bo)
    lib.gbm_bo_destroy(bo)
    lib.gbm_device_destroy(a)
    if separate_fd:
        os.close(fd2)
    os.close(fd1)
    print('PASS', 'independent DRM fds' if separate_fd else 'shared DRM fd', flush=True)
