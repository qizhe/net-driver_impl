source param.sh

# Configuration
~/NetChannel/scripts/run_np.sh $iface $1

# Run the client program
sudo taskset -c 0 ~/NetChannel/util/iouring_bench_nc client-shortflows $server_ip 9095 60
