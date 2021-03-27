const fs = require('fs');
const pickle = require('chromium-pickle-js')
const Filesystem = require('./filesystem')
const disk = require('./disk')

module.exports.encryptAll = async function (archive, dest) {
  const filesystem = disk.readFilesystemSync(archive)
  const filenames = filesystem.listFiles()

  const headerPickle = pickle.createEmpty()
  headerPickle.writeString(JSON.stringify(filesystem.header))
  const headerBuf = headerPickle.toBuffer()

  const sizePickle = pickle.createEmpty()
  sizePickle.writeUInt32(headerBuf.length)
  const sizeBuf = sizePickle.toBuffer()
  var out = fs.createWriteStream(dest)
  const fd = fs.openSync(archive, 'r');
  out.write(sizeBuf)
  out.write(headerBuf)
  for (const info in filesystem.header.files) {
    console.log(info,filesystem.header.files[info])
    const {size, offset} = filesystem.header.files[info];
    let filebuf = Buffer.alloc(size);
    fs.readSync(fd, filebuf, 0, size, filesystem.headerSize+offset)
    for (let i = 0; i < filebuf.length; i++) {
      filebuf[i] = filebuf[i] ^ 193;
    }
    out.write(filebuf)
  }
  out.end()

  // await new Promise((resolve, reject) => {
  //   out.on('error', reject)
  //   out.write(sizeBuf)
  //   out.write(headerBuf)
  //   return out.write(filebuf, () => resolve())
  // })
}

