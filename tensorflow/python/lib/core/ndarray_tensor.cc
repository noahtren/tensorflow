/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/python/lib/core/ndarray_tensor.h"

#include <cstring>

#include "tensorflow/core/lib/core/coding.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/python/lib/core/ndarray_tensor_bridge.h"
#include "tensorflow/python/lib/core/ndarray_tensor_types.h"
#include "tensorflow/python/lib/core/numpy.h"

namespace tensorflow {
namespace {

Status PyObjectToString(PyObject* obj, const char** ptr, Py_ssize_t* len,
                        PyObject** ptr_owner) {
  *ptr_owner = nullptr;
  if (PyBytes_Check(obj)) {
    char* buf;
    if (PyBytes_AsStringAndSize(obj, &buf, len) != 0) {
      return errors::Internal("Unable to get element as bytes.");
    }
    *ptr = buf;
    return Status::OK();
  } else if (PyUnicode_Check(obj)) {
#if (PY_MAJOR_VERSION > 3 || (PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 3))
    *ptr = PyUnicode_AsUTF8AndSize(obj, len);
    if (*ptr != nullptr) return Status::OK();
#else
    PyObject* utemp = PyUnicode_AsUTF8String(obj);
    char* buf;
    if (utemp != nullptr && PyBytes_AsStringAndSize(utemp, &buf, len) != -1) {
      *ptr = buf;
      *ptr_owner = utemp;
      return Status::OK();
    }
    Py_XDECREF(utemp);
#endif
    return errors::Internal("Unable to convert element to UTF-8");
  } else {
    return errors::Internal("Unsupported object type ", obj->ob_type->tp_name);
  }
}

// Iterate over the string array 'array', extract the ptr and len of each string
// element and call f(ptr, len).
template <typename F>
Status PyBytesArrayMap(PyArrayObject* array, F f) {
  Safe_PyObjectPtr iter = tensorflow::make_safe(
      PyArray_IterNew(reinterpret_cast<PyObject*>(array)));
  while (PyArray_ITER_NOTDONE(iter.get())) {
    auto item = tensorflow::make_safe(PyArray_GETITEM(
        array, static_cast<char*>(PyArray_ITER_DATA(iter.get()))));
    if (!item) {
      return errors::Internal("Unable to get element from the feed - no item.");
    }
    Py_ssize_t len;
    const char* ptr;
    PyObject* ptr_owner = nullptr;
    TF_RETURN_IF_ERROR(PyObjectToString(item.get(), &ptr, &len, &ptr_owner));
    f(ptr, len);
    Py_XDECREF(ptr_owner);
    PyArray_ITER_NEXT(iter.get());
  }
  return Status::OK();
}

// Encode the strings in 'array' into a contiguous buffer and return the base of
// the buffer. The caller takes ownership of the buffer.
Status EncodePyBytesArray(PyArrayObject* array, tensorflow::int64 nelems,
                          size_t* size, void** buffer) {
  // Compute bytes needed for encoding.
  *size = 0;
  TF_RETURN_IF_ERROR(
      PyBytesArrayMap(array, [&size](const char* ptr, Py_ssize_t len) {
        *size += sizeof(tensorflow::uint64) +
                 tensorflow::core::VarintLength(len) + len;
      }));
  // Encode all strings.
  std::unique_ptr<char[]> base_ptr(new char[*size]);
  char* base = base_ptr.get();
  char* data_start = base + sizeof(tensorflow::uint64) * nelems;
  char* dst = data_start;  // Where next string is encoded.
  tensorflow::uint64* offsets = reinterpret_cast<tensorflow::uint64*>(base);

  TF_RETURN_IF_ERROR(PyBytesArrayMap(
      array, [&data_start, &dst, &offsets](const char* ptr, Py_ssize_t len) {
        *offsets = (dst - data_start);
        offsets++;
        dst = tensorflow::core::EncodeVarint64(dst, len);
        memcpy(dst, ptr, len);
        dst += len;
      }));
  CHECK_EQ(dst, base + *size);
  *buffer = base_ptr.release();
  return Status::OK();
}

Status CopyTF_TensorStringsToPyArray(const TF_Tensor* src, uint64 nelems,
                                     PyArrayObject* dst) {
  const void* tensor_data = TF_TensorData(src);
  const size_t tensor_size = TF_TensorByteSize(src);
  const char* limit = static_cast<const char*>(tensor_data) + tensor_size;
  DCHECK(tensor_data != nullptr);
  DCHECK_EQ(TF_STRING, TF_TensorType(src));

  const uint64* offsets = static_cast<const uint64*>(tensor_data);
  const size_t offsets_size = sizeof(uint64) * nelems;
  const char* data = static_cast<const char*>(tensor_data) + offsets_size;

  const size_t expected_tensor_size =
      (limit - static_cast<const char*>(tensor_data));
  if (expected_tensor_size - tensor_size) {
    return errors::InvalidArgument(
        "Invalid/corrupt TF_STRING tensor: expected ", expected_tensor_size,
        " bytes of encoded strings for the tensor containing ", nelems,
        " strings, but the tensor is encoded in ", tensor_size, " bytes");
  }
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  auto iter = make_safe(PyArray_IterNew(reinterpret_cast<PyObject*>(dst)));
  for (int64 i = 0; i < nelems; ++i) {
    const char* start = data + offsets[i];
    const char* ptr = nullptr;
    size_t len = 0;

    TF_StringDecode(start, limit - start, &ptr, &len, status.get());
    if (TF_GetCode(status.get()) != TF_OK) {
      return errors::InvalidArgument(TF_Message(status.get()));
    }

    auto py_string = make_safe(PyBytes_FromStringAndSize(ptr, len));
    if (py_string == nullptr) {
      return errors::Internal(
          "failed to create a python byte array when converting element #", i,
          " of a TF_STRING tensor to a numpy ndarray");
    }

    if (PyArray_SETITEM(dst, static_cast<char*>(PyArray_ITER_DATA(iter.get())),
                        py_string.get()) != 0) {
      return errors::Internal("Error settings element #", i,
                              " in the numpy ndarray");
    }
    PyArray_ITER_NEXT(iter.get());
  }
  return Status::OK();
}

// Determine the dimensions of a numpy ndarray to be created to represent an
// output Tensor.
Status GetPyArrayDimensionsForTensor(const TF_Tensor* tensor,
                                     gtl::InlinedVector<npy_intp, 4>* dims,
                                     tensorflow::int64* nelems) {
  dims->clear();
  const int ndims = TF_NumDims(tensor);
  if (TF_TensorType(tensor) == TF_RESOURCE) {
    if (ndims != 0) {
      return errors::InvalidArgument(
          "Fetching of non-scalar resource tensors is not supported.");
    }
    dims->push_back(TF_TensorByteSize(tensor));
    *nelems = dims->back();
  } else {
    *nelems = 1;
    for (int i = 0; i < ndims; ++i) {
      dims->push_back(TF_Dim(tensor, i));
      *nelems *= dims->back();
    }
  }
  return Status::OK();
}

inline void FastMemcpy(void* dst, const void* src, size_t size) {
  // clang-format off
  switch (size) {
    // Most compilers will generate inline code for fixed sizes,
    // which is significantly faster for small copies.
    case  1: memcpy(dst, src, 1); break;
    case  2: memcpy(dst, src, 2); break;
    case  3: memcpy(dst, src, 3); break;
    case  4: memcpy(dst, src, 4); break;
    case  5: memcpy(dst, src, 5); break;
    case  6: memcpy(dst, src, 6); break;
    case  7: memcpy(dst, src, 7); break;
    case  8: memcpy(dst, src, 8); break;
    case  9: memcpy(dst, src, 9); break;
    case 10: memcpy(dst, src, 10); break;
    case 11: memcpy(dst, src, 11); break;
    case 12: memcpy(dst, src, 12); break;
    case 13: memcpy(dst, src, 13); break;
    case 14: memcpy(dst, src, 14); break;
    case 15: memcpy(dst, src, 15); break;
    case 16: memcpy(dst, src, 16); break;
#if defined(PLATFORM_GOOGLE) || defined(PLATFORM_POSIX) && \
    !defined(IS_MOBILE_PLATFORM)
    // On Linux, memmove appears to be faster than memcpy for
    // large sizes, strangely enough.
    default: memmove(dst, src, size); break;
#else
    default: memcpy(dst, src, size); break;
#endif
  }
  // clang-format on
}

}  // namespace

// TODO(slebedev): revise TF_TensorToPyArray usages and switch to the
// aliased version where appropriate.
Status TF_TensorToMaybeAliasedPyArray(Safe_TF_TensorPtr tensor,
                                      PyObject** out_ndarray) {
  auto dtype = TF_TensorType(tensor.get());
  if (dtype == TF_STRING || dtype == TF_RESOURCE) {
    return TF_TensorToPyArray(std::move(tensor), out_ndarray);
  }

  TF_Tensor* moved = tensor.release();
  int64 nelems = -1;
  gtl::InlinedVector<npy_intp, 4> dims;
  TF_RETURN_IF_ERROR(GetPyArrayDimensionsForTensor(moved, &dims, &nelems));
  return ArrayFromMemory(
      dims.size(), dims.data(), TF_TensorData(moved),
      static_cast<DataType>(dtype), [moved] { TF_DeleteTensor(moved); },
      out_ndarray);
}

// Converts the given TF_Tensor to a numpy ndarray.
// If the returned status is OK, the caller becomes the owner of *out_array.
Status TF_TensorToPyArray(Safe_TF_TensorPtr tensor, PyObject** out_ndarray) {
  // A fetched operation will correspond to a null tensor, and a None
  // in Python.
  if (tensor == nullptr) {
    Py_INCREF(Py_None);
    *out_ndarray = Py_None;
    return Status::OK();
  }
  int64 nelems = -1;
  gtl::InlinedVector<npy_intp, 4> dims;
  TF_RETURN_IF_ERROR(
      GetPyArrayDimensionsForTensor(tensor.get(), &dims, &nelems));

  // If the type is neither string nor resource we can reuse the Tensor memory.
  TF_Tensor* original = tensor.get();
  TF_Tensor* moved = TF_TensorMaybeMove(tensor.release());
  if (moved != nullptr) {
    if (ArrayFromMemory(
            dims.size(), dims.data(), TF_TensorData(moved),
            static_cast<DataType>(TF_TensorType(moved)),
            [moved] { TF_DeleteTensor(moved); }, out_ndarray)
            .ok()) {
      return Status::OK();
    }
  }
  tensor.reset(original);

  // Copy the TF_TensorData into a newly-created ndarray and return it.
  PyArray_Descr* descr = nullptr;
  TF_RETURN_IF_ERROR(DataTypeToPyArray_Descr(
      static_cast<DataType>(TF_TensorType(tensor.get())), &descr));
  Safe_PyObjectPtr safe_out_array =
      tensorflow::make_safe(PyArray_Empty(dims.size(), dims.data(), descr, 0));
  if (!safe_out_array) {
    return errors::Internal("Could not allocate ndarray");
  }
  PyArrayObject* py_array =
      reinterpret_cast<PyArrayObject*>(safe_out_array.get());
  if (TF_TensorType(tensor.get()) == TF_STRING) {
    Status s = CopyTF_TensorStringsToPyArray(tensor.get(), nelems, py_array);
    if (!s.ok()) {
      return s;
    }
  } else if (static_cast<size_t>(PyArray_NBYTES(py_array)) !=
             TF_TensorByteSize(tensor.get())) {
    return errors::Internal("ndarray was ", PyArray_NBYTES(py_array),
                            " bytes but TF_Tensor was ",
                            TF_TensorByteSize(tensor.get()), " bytes");
  } else {
    FastMemcpy(PyArray_DATA(py_array), TF_TensorData(tensor.get()),
               PyArray_NBYTES(py_array));
  }

  *out_ndarray = safe_out_array.release();
  return Status::OK();
}

Status PyArrayToTF_Tensor(PyObject* ndarray, Safe_TF_TensorPtr* out_tensor) {
  DCHECK(out_tensor != nullptr);

  // Make sure we dereference this array object in case of error, etc.
  Safe_PyObjectPtr array_safe(make_safe(
      PyArray_FromAny(ndarray, nullptr, 0, 0, NPY_ARRAY_CARRAY_RO, nullptr)));
  if (!array_safe) return errors::InvalidArgument("Not a ndarray.");
  PyArrayObject* array = reinterpret_cast<PyArrayObject*>(array_safe.get());

  // Convert numpy dtype to TensorFlow dtype.
  TF_DataType dtype = TF_FLOAT;
  {
    DataType tmp;
    TF_RETURN_IF_ERROR(PyArray_DescrToDataType(PyArray_DESCR(array), &tmp));
    dtype = static_cast<TF_DataType>(tmp);
  }

  tensorflow::int64 nelems = 1;
  gtl::InlinedVector<int64_t, 4> dims;
  for (int i = 0; i < PyArray_NDIM(array); ++i) {
    dims.push_back(PyArray_SHAPE(array)[i]);
    nelems *= dims[i];
  }

  // Create a TF_Tensor based on the fed data. In the case of non-string data
  // type, this steals a reference to array, which will be relinquished when
  // the underlying buffer is deallocated. For string, a new temporary buffer
  // is allocated into which the strings are encoded.
  if (dtype == TF_RESOURCE) {
    size_t size = PyArray_NBYTES(array);
    array_safe.release();
    *out_tensor = make_safe(TF_NewTensor(dtype, {}, 0, PyArray_DATA(array),
                                         size, &DelayedNumpyDecref, array));

  } else if (dtype != TF_STRING) {
    size_t size = PyArray_NBYTES(array);
    array_safe.release();
    *out_tensor = make_safe(TF_NewTensor(dtype, dims.data(), dims.size(),
                                         PyArray_DATA(array), size,
                                         &DelayedNumpyDecref, array));
  } else {
    size_t size = 0;
    void* encoded = nullptr;
    TF_RETURN_IF_ERROR(EncodePyBytesArray(array, nelems, &size, &encoded));
    *out_tensor = make_safe(TF_NewTensor(
        dtype, dims.data(), dims.size(), encoded, size,
        [](void* data, size_t len, void* arg) {
          delete[] reinterpret_cast<char*>(data);
        },
        nullptr));
  }
  return Status::OK();
}

Status TF_TensorToTensor(const TF_Tensor* src, Tensor* dst);
TF_Tensor* TF_TensorFromTensor(const tensorflow::Tensor& src,
                               TF_Status* status);

Status NdarrayToTensor(PyObject* obj, Tensor* ret) {
  Safe_TF_TensorPtr tf_tensor = make_safe(static_cast<TF_Tensor*>(nullptr));
  Status s = PyArrayToTF_Tensor(obj, &tf_tensor);
  if (!s.ok()) {
    return s;
  }
  return TF_TensorToTensor(tf_tensor.get(), ret);
}

Status TensorToNdarray(const Tensor& t, PyObject** ret) {
  TF_Status* status = TF_NewStatus();
  Safe_TF_TensorPtr tf_tensor = make_safe(TF_TensorFromTensor(t, status));
  Status tf_status = StatusFromTF_Status(status);
  TF_DeleteStatus(status);
  if (!tf_status.ok()) {
    return tf_status;
  }
  return TF_TensorToPyArray(std::move(tf_tensor), ret);
}

}  // namespace tensorflow
