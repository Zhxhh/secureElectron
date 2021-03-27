const crypto = require('crypto');
const asar = require('../lib/asar');
const key = "testtesttesttest"; // aes-128:key是16字节
const algorithm = 'aes-128-ecb';

(async function() { 
  await asar.createPackageWithOptions('dist', 'out/enc.asar', {
    transform: (filePath) => {
      return crypto.createCipher('aes-128-ecb',key).setEncoding('base64')
    }
  });
})();
