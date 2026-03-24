# client_pckg

1. Install Dependencies
Run this on the new laptop to install the required gRPC and Protobuf libraries:

bash
sudo apt update
sudo apt install -y libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc build-essential pkg-config
2. Clone and Compile
Once you have cloned the repository, move into the folder and build the client to ensure it matches the new laptop's hardware/OS:

bash
cd client_pckg
chmod +x compile_client.sh run_client.sh
./compile_client.sh
3. Run the Benchmark
Use the 

run_client.sh
 script. If the server is using SSL, make sure to include the Target Name Override (the name inside the server's certificate) as the 5th argument.

Example: 400 threads, 60 seconds, against server at 192.168.0.84

bash
# Template: ./run_client.sh <THREADS> <SECONDS> <SERVER_IP> [PAYLOAD] [SSL_OVERRIDE]
./run_client.sh 400 60 192.168.0.84 64 kv-server
