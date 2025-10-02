mkdir build
cd build
cmake -G "Ninja" -DPICO_BOARD=pico_w ..
cd ..
mkdir vs-proj
cd vs-proj
cmake -DPICO_BOARD=pico_w ..
cd ..
