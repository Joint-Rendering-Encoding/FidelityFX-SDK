@echo off

:: Clone moq-rs repository
IF NOT EXIST build\moq-rs (
    git clone https://github.com/kixelated/moq-rs build\moq-rs
)

:: Checkout the correct commit
cd build\moq-rs
git checkout 97f27996cea4972cf0a51b187d6b0321297d60ad

:: Build moq-rs
cargo build --release

:: Copy moq-pub and moq-relay to the bin directory
copy target\release\moq-pub.exe ..\..\bin
copy target\release\moq-relay.exe ..\..\bin

:: Generate certificates
cd dev
go run filippo.io/mkcert -ecdsa -install
go run filippo.io/mkcert -ecdsa -days 10 -cert-file localhost.crt -key-file localhost.key localhost 127.0.0.1 ::1

:: Copy certificates to the bin directory
mkdir ..\..\..\bin\cert
copy localhost.crt ..\..\..\bin\cert\localhost.crt
copy localhost.key ..\..\..\bin\cert\localhost.key

:: Return to the root directory
cd ..\..\..

:: Build the demo page
cd demo
npm install && npm run build
cd ..
