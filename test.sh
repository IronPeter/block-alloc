echo "build bench with block alloc"
g++ -O2 main.cpp alloc.cpp -o bench_blk
echo "done. run bench"
time ./bench_blk
echo "build bench with std alloc"
g++ -O2 main.cpp -o bench_std
echo "done. run bench"
time ./bench_std

