#!/usr/bin/env bash
# compile_client.sh - Compiles the load_client on the second laptop

echo "Checking for protoc and grpc..."
if ! command -v protoc &> /dev/null; then
    echo "Error: protoc not found. Please install protobuf and gRPC."
    exit 1
fi

echo "Generating proto code..."
mkdir -p proto_gen
protoc --proto_path=. --cpp_out=proto_gen --grpc_out=proto_gen --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) kv.proto

echo "Compiling load_client..."
g++ -O3 -std=c++17 -I./proto_gen \
    load_client.cpp \
    proto_gen/kv.pb.cc \
    proto_gen/kv.grpc.pb.cc \
    $(pkg-config --static --libs --cflags grpc++ protobuf) \
    -lpthread \
    -o load_client

if [ $? -eq 0 ]; then
    echo "Successfully compiled load_client!"
    chmod +x load_client
else
    echo "Compilation failed. Check if gRPC/Protobuf development libraries are installed."
fi
