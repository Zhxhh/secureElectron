// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <vector>

#include "base/numerics/safe_math.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "gin/wrappable.h"
#include "shell/common/asar/archive.h"
#include "shell/common/asar/asar_util.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/error_thrower.h"
#include "shell/common/gin_helper/function_template_extensions.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/node_includes.h"
#include "shell/common/node_util.h"

#include <string>
#include <openssl/evp.h>
#include <openssl/md5.h>
typedef std::vector<uint8_t> byte_array;

namespace {

class Archive : public gin::Wrappable<Archive> {
 public:
  static gin::Handle<Archive> Create(v8::Isolate* isolate,
                                     const base::FilePath& path) {
    auto archive = std::make_unique<asar::Archive>(path);
    if (!archive->Init())
      return gin::Handle<Archive>();
    return gin::CreateHandle(isolate, new Archive(isolate, std::move(archive)));
  }

  // gin::Wrappable
  static gin::WrapperInfo kWrapperInfo;
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override {
    return gin::ObjectTemplateBuilder(isolate)
        .SetProperty("path", &Archive::GetPath)
        .SetMethod("getFileInfo", &Archive::GetFileInfo)
        .SetMethod("stat", &Archive::Stat)
        .SetMethod("readdir", &Archive::Readdir)
        .SetMethod("realpath", &Archive::Realpath)
        .SetMethod("copyFileOut", &Archive::CopyFileOut)
        .SetMethod("read", &Archive::Read)
        .SetMethod("readSync", &Archive::ReadSync);
  }

  const char* GetTypeName() override { return "Archive"; }

 protected:
  Archive(v8::Isolate* isolate, std::unique_ptr<asar::Archive> archive)
      : archive_(std::move(archive)) {}

  // Returns the path of the file.
  base::FilePath GetPath() { return archive_->path(); }

  // Reads the offset and size of file.
  v8::Local<v8::Value> GetFileInfo(v8::Isolate* isolate,
                                   const base::FilePath& path) {
    asar::Archive::FileInfo info;
    if (!archive_ || !archive_->GetFileInfo(path, &info))
      return v8::False(isolate);
    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", info.size);
    dict.Set("unpacked", info.unpacked);
    dict.Set("offset", info.offset);
    dict.Set("encrypted", info.encrypted);
    dict.Set("len", info.len);
    return dict.GetHandle();
  }

  // Returns a fake result of fs.stat(path).
  v8::Local<v8::Value> Stat(v8::Isolate* isolate, const base::FilePath& path) {
    asar::Archive::Stats stats;
    if (!archive_ || !archive_->Stat(path, &stats))
      return v8::False(isolate);
    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", stats.size);
    dict.Set("offset", stats.offset);
    dict.Set("isFile", stats.is_file);
    dict.Set("isDirectory", stats.is_directory);
    dict.Set("isLink", stats.is_link);
    return dict.GetHandle();
  }

  // Returns all files under a directory.
  v8::Local<v8::Value> Readdir(v8::Isolate* isolate,
                               const base::FilePath& path) {
    std::vector<base::FilePath> files;
    if (!archive_ || !archive_->Readdir(path, &files))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, files);
  }

  // Returns the path of file with symbol link resolved.
  v8::Local<v8::Value> Realpath(v8::Isolate* isolate,
                                const base::FilePath& path) {
    base::FilePath realpath;
    if (!archive_ || !archive_->Realpath(path, &realpath))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, realpath);
  }

  // Copy the file out into a temporary file and returns the new path.
  v8::Local<v8::Value> CopyFileOut(v8::Isolate* isolate,
                                   const base::FilePath& path) {
    base::FilePath new_path;
    if (!archive_ || !archive_->CopyFileOut(path, &new_path))
      return v8::False(isolate);
    return gin::ConvertToV8(isolate, new_path);
  }

  v8::Local<v8::ArrayBuffer> ReadSync(gin_helper::ErrorThrower thrower,
                                      uint64_t offset,
                                      uint64_t length) {
    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > archive_->file()->length()) {
      thrower.ThrowError("Out of bounds read");
      return v8::Local<v8::ArrayBuffer>();
    }
    auto array_buffer = v8::ArrayBuffer::New(thrower.isolate(), length);
    auto backing_store = array_buffer->GetBackingStore();
    memcpy(backing_store->Data(), archive_->file()->data() + offset, length);
    return array_buffer;
  }

  v8::Local<v8::Promise> Read(v8::Isolate* isolate,
                              uint64_t offset,
                              uint64_t length) {
    gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise(isolate);
    v8::Local<v8::Promise> handle = promise.GetHandle();

    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > archive_->file()->length()) {
      promise.RejectWithErrorMessage("Out of bounds read");
      return handle;
    }

    auto backing_store = v8::ArrayBuffer::NewBackingStore(isolate, length);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Archive::ReadOnIO, isolate, archive_,
                       std::move(backing_store), offset, length),
        base::BindOnce(&Archive::ResolveReadOnUI, std::move(promise)));

    return handle;
  }

 private:
  static std::unique_ptr<v8::BackingStore> ReadOnIO(
      v8::Isolate* isolate,
      std::shared_ptr<asar::Archive> archive,
      std::unique_ptr<v8::BackingStore> backing_store,
      uint64_t offset,
      uint64_t length) {
    memcpy(backing_store->Data(), archive->file()->data() + offset, length);
    return backing_store;
  }

  static void ResolveReadOnUI(
      gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise,
      std::unique_ptr<v8::BackingStore> backing_store) {
    v8::HandleScope scope(promise.isolate());
    v8::Context::Scope context_scope(promise.GetContext());
    auto array_buffer =
        v8::ArrayBuffer::New(promise.isolate(), std::move(backing_store));
    promise.Resolve(array_buffer);
  }

  std::shared_ptr<asar::Archive> archive_;

  DISALLOW_COPY_AND_ASSIGN(Archive);
};

// static
gin::WrapperInfo Archive::kWrapperInfo = {gin::kEmbedderNativeGin};

void InitAsarSupport(v8::Isolate* isolate, v8::Local<v8::Value> require) {
  // Evaluate asar_bundle.js.
  std::vector<v8::Local<v8::String>> asar_bundle_params = {
      node::FIXED_ONE_BYTE_STRING(isolate, "require")};
  std::vector<v8::Local<v8::Value>> asar_bundle_args = {require};
  electron::util::CompileAndCall(
      isolate->GetCurrentContext(), "electron/js2c/asar_bundle",
      &asar_bundle_params, &asar_bundle_args, nullptr);
}

v8::Local<v8::Value> SplitPath(v8::Isolate* isolate,
                               const base::FilePath& path) {
  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  base::FilePath asar_path, file_path;
  if (asar::GetAsarArchivePath(path, &asar_path, &file_path, true)) {
    dict.Set("isAsar", true);
    dict.Set("asarPath", asar_path);
    dict.Set("filePath", file_path);
  } else {
    dict.Set("isAsar", false);
  }
  return dict.GetHandle();
}

v8::Local<v8::ArrayBuffer> DecodeBuffer(v8::Isolate* isolate,
                                  v8::Local<v8::Value> buffer, int len) {
  v8::Local<v8::ArrayBuffer> _buffer =
      v8::Local<v8::ArrayBuffer>::Cast(buffer);

  //base64??????
  int size = _buffer->ByteLength();
  byte_array out;
  out.resize(size);
  size = EVP_DecodeBlock(&out[0], 
      reinterpret_cast<unsigned char*>((char*)_buffer->GetContents().Data()), 
      size);
  out.resize(size);
  unsigned char encData[out.size()];
  memcpy(encData, &out[0], out.size());

  //aes-128-ecb??????
  std::string g_key = "testtesttesttest";
  unsigned char key[16] = { 0 };
  int outlen = 0;
  unsigned char decData[len];
  std::copy(g_key.begin(), g_key.end(), key);
  MD5((unsigned char*)g_key.c_str(), g_key.length(), key);
  //??????
  int decLen = 0;
  EVP_CIPHER_CTX *ctx2;
  ctx2 = EVP_CIPHER_CTX_new();
  EVP_CipherInit_ex(ctx2, EVP_aes_128_ecb(), NULL, key, nullptr, 0);
  while (decLen < len) {
    EVP_CipherUpdate(ctx2, decData + decLen, &outlen, encData, out.size()-decLen);
    decLen += outlen;
    EVP_CipherFinal_ex(ctx2, decData + decLen + outlen, &outlen);
    decLen += outlen;
  }
  EVP_CIPHER_CTX_free(ctx2);
  auto array_buffer = v8::ArrayBuffer::New(isolate, len);
  auto backing_store = array_buffer->GetBackingStore();
  memcpy(backing_store->Data(), decData, len);
  return array_buffer;
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin_helper::Dictionary dict(context->GetIsolate(), exports);
  dict.SetMethod("createArchive", &Archive::Create);
  dict.SetMethod("splitPath", &SplitPath);
  dict.SetMethod("initAsarSupport", &InitAsarSupport);
  dict.SetMethod("decodeBuffer", &DecodeBuffer);
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(electron_common_asar, Initialize)
