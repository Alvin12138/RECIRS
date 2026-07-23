#!/bin/bash
if [ $# -eq 0 ]; then
    echo "Error: You must provide a file to be compiled as argument"
    exit 1
fi

#move into the scripts folder if you're not there already
cd scripts &>/dev/null

OPENPLC_PLATFORM=$(cat openplc_platform)
ETHERCAT_OPT=$(cat ethercat)
OPENPLC_DRIVER=$(cat openplc_driver)

#store the active program filename
echo "$1" > ../active_program

#compiling the ST file into C
cd ..
echo "Generating C files..."
./iec2c -f -l -p -r -R -a ./st_files/"$1"
if [ $? -ne 0 ]; then
    echo "Error generating C files"
    echo "Compilation finished with errors!"
    exit 1
fi

CFI_ENABLE=1
if [ "$CFI_ENABLE" -eq 1 ]; then
    echo "=== CFI Protection ENABLED (Backward-edge + Forward-edge with runtime learning) ==="
    sed -i '1i#include "openplc_cfi.h"\n#define CFI_PROTECT 1' POUS.c
    sed -i '1i#include "openplc_cfi.h"\n#define CFI_PROTECT 1' Config0.c
    sed -i '1i#include "openplc_cfi.h"\n#define CFI_PROTECT 1' Res0.c
    for file in core/main.cpp core/modbus.cpp core/modbus_master.cpp core/enip.cpp core/server.cpp core/interactive_server.cpp core/dnp3.cpp core/pccc.cpp core/persistent_storage.cpp core/utils.cpp; do
        if grep -q '^#define CFI_PROTECT' "$file"; then
	    sed -i 's/^#define CFI_PROTECT 0/#define CFI_PROTECT 1/' "$file"
        else
            sed -i '1i#define CFI_PROTECT 1' "$file"
        fi
	if grep -q '^#include "openplc_cfi.h"' "$file"; then
	    echo "openplc_cfi.h is included."
	else
	    sed -i '1i#include "openplc_cfi.h"' "$file"
	fi
	if grep -q 'CFI_PROLOGUE_TAG' "$file" || [ "$file" = core/main.cpp ]; then
	    echo "CFI_PROLOGUE_TAG already."
	else
            python3 core/cfi_instrument.py --all "$file"   
	fi
    done
    # Backward-edge instrumentation (function entry/exit)+Forward-edge instrumentation (pthread_create call point)
    python3 core/cfi_instrument.py --all POUS.c Config0.c Res0.c
    echo "Forward-edge CFI: runtime learning mode (no pre-compiled tables needed)"
else
    for file in core/main.cpp core/modbus.cpp core/modbus_master.cpp core/enip.cpp core/server.cpp core/interactive_server.cpp core/dnp3.cpp core/pccc.cpp core/persistent_storage.cpp core/utils.cpp; do
        if grep -q '^#define CFI_PROTECT' "$file"; then
	    sed -i 's/^#define CFI_PROTECT 1/#define CFI_PROTECT 0/' "$file"
        else
            sed -i '1i#define CFI_PROTECT 0' "$file"
        fi
    done
fi

CFI_TIMING=1
PAC_ENABLE=1
# PAC_LEVEL="pac-ret"
PAC_LEVEL="standard"
PAC_CFLAGS=""
PAC_CXXFLAGS=""
PAC_LDFLAGS=""
if [ "$PAC_ENABLE" -eq 1 ]; then
    PAC_CFLAGS="-mbranch-protection=${PAC_LEVEL} -fno-plt -DCFI_TIMING=${CFI_TIMING}"
    PAC_CXXFLAGS="-mbranch-protection=${PAC_LEVEL}"
    # PAC_LDFLAGS="-Wl,-z,pac-plt"
    PAC_LDFLAGS="-Wl,-z,bti-report=warning"
    echo "=== PAC Pointer Authentication ENABLED (level: ${PAC_LEVEL}) ==="
fi

# stick reference to ethercat_src in there for CoE access etc functionality that needs to be accessed from PLC
if [ "$ETHERCAT_OPT" = "ethercat" ]; then
    sed -i '7s/^/#include "ethercat_src.h" /' Res0.c
fi

# I prefer copying every time these two (small files) because could be useful to have a copy of them for testing
echo "Including Siemens S7 Protocol via snap7"
cp -f ../utils/snap7_src/wrapper/oplc_snap7.* ./core

echo "Moving Files..."
mv -f POUS.c POUS.h LOCATED_VARIABLES.h VARIABLES.csv Config0.c Config0.h Res0.c ./core/
if [ $? -ne 0 ]; then
    echo "Error moving files"
    echo "Compilation finished with errors!"
    exit 1
fi

if [ "$ETHERCAT_OPT" = "ethercat" ]; then
    echo "Including EtherCAT"
    ETHERCAT_INC="-L../../utils/ethercat_src/build/lib -lethercat_src -I../../utils/ethercat_src/src -D _ethercat_src"
else
    ETHERCAT_INC=""
fi

#compiling for each platform
cd core
if [ "$OPENPLC_PLATFORM" = "win" ]; then
    echo "Compiling for Windows"
    echo "Generating object files..."
    g++ -I ./lib -c Config0.c -w
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    g++ -I ./lib -c Res0.c -w
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Generating glueVars..."
    ./glue_generator
    echo "Compiling main program..."
    g++ *.cpp *.o -o openplc -I ./lib -pthread -fpermissive -I /usr/local/include/modbus -L /usr/local/lib snap7.lib -lmodbus -w 
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Compilation finished successfully!"
    exit 0
    
elif [ "$OPENPLC_PLATFORM" = "linux" ]; then
    if [ "$CFI_ENABLE" -eq 1 ]; then
        echo "Compiling for Linux with CFI (Backward + Forward)"
        echo "Compiling CFI shadow stack module..."
        gcc -I ./lib -I/usr/include -O2 -c openplc_got.c -o openplc_got.o ${PAC_CFLAGS} -lcrypto
        gcc -I ./lib -I/usr/include -O2 -c openplc_cfi.c -o openplc_cfi.o ${PAC_CFLAGS} -lcrypto
        if [ $? -ne 0 ]; then
            echo "Error compiling CFI module"
            exit 1
        fi
        echo "Forward-edge CFI target table: core/cfi_forward_target.h"
    fi
    echo "Generating object files..."
    if [ "$OPENPLC_DRIVER" = "sl_rp4" ]; then
        g++ -std=gnu++11 -I ./lib -O2 -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w -DSL_RP4 ${PAC_CFLAGS}
    elif [ "$OPENPLC_DRIVER" = "synergy_logic" ]; then
        g++ -std=gnu++11 -I ./lib -O2 -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w -DSYNERGY ${PAC_CFLAGS}
    else
        g++ -std=gnu++11 -I ./lib -O2 -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w ${PAC_CFLAGS}
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    if [ "$OPENPLC_DRIVER" = "sl_rp4" ]; then
        g++ -std=gnu++11 -I ./lib -O2 -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $ETHERCAT_INC -DSL_RP4 ${PAC_CFLAGS}
    elif [ "$OPENPLC_DRIVER" = "synergy_logic" ]; then
        g++ -std=gnu++11 -I ./lib -O2 -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $ETHERCAT_INC -DSYNERGY ${PAC_CFLAGS}
    else
        g++ -std=gnu++11 -I ./lib -O2 -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $ETHERCAT_INC ${PAC_CFLAGS}
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Generating glueVars..."
    ./glue_generator
    echo "Compiling main program..."
    if [ "$OPENPLC_DRIVER" = "sl_rp4" ]; then
        g++ -std=gnu++11 -O2 *.cpp *.o -o openplc -I ./lib -lteec -pthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -lrt -w $ETHERCAT_INC -DSL_RP4 ${PAC_CXXFLAGS} ${PAC_LDFLAGS} -lcrypto
    elif [ "$OPENPLC_DRIVER" = "synergy_logic" ]; then
        g++ -std=gnu++11 -O2 *.cpp *.o -o openplc -I ./lib -lteec -pthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -lrt -w $ETHERCAT_INC -DSYNERGY ${PAC_CXXFLAGS} ${PAC_LDFLAGS} -lcrypto
    else
        g++ -std=gnu++11 -O2 *.cpp *.o -o openplc -I ./lib -lteec -pthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -lrt -w $ETHERCAT_INC ${PAC_CXXFLAGS} ${PAC_LDFLAGS} -lcrypto
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Compilation finished successfully!"
    exit 0
    
elif [ "$OPENPLC_PLATFORM" = "rpi" ]; then
    echo "Compiling for Raspberry Pi"
    echo "Generating object files..."
    if [ "$OPENPLC_DRIVER" = "sequent" ]; then
        g++ -std=gnu++11 -I ./lib -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w -DSEQUENT
    else
        g++ -std=gnu++11 -I ./lib -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    if [ "$OPENPLC_DRIVER" = "sequent" ]; then
        g++ -std=gnu++11 -I ./lib -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w -DSEQUENT
    else
        g++ -std=gnu++11 -I ./lib -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Generating glueVars..."
    ./glue_generator
    echo "Compiling main program..."
    if [ "$OPENPLC_DRIVER" = "sequent" ]; then
        g++ -DSEQUENT -std=gnu++11 *.cpp *.o -o openplc -I ./lib -lrt -lwiringPi -lpthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w 
    else
        g++ -std=gnu++11 *.cpp *.o -o openplc -I ./lib -lrt -lwiringPi -lpthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w
    fi
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Compilation finished successfully!"
    exit 0

elif [ "$OPENPLC_PLATFORM" = "opi" ]; then
    WIRINGOP_INC="-I/usr/local/include -L/usr/local/lib -lwiringPi -lwiringPiDev"
    echo "Compiling for Orange Pi"
    echo "Generating object files..."
    g++ -std=gnu++11 -I ./lib -c Config0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $WIRINGOP_INC
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    g++ -std=gnu++11 -I ./lib -c Res0.c -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $WIRINGOP_INC
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Generating glueVars..."
    ./glue_generator
    echo "Compiling main program..."
    g++ -std=gnu++11 *.cpp *.o -o openplc -I ./lib -lrt -lpthread -fpermissive `pkg-config --cflags --libs libmodbus` -lsnap7 -lasiodnp3 -lasiopal -lopendnp3 -lopenpal -w $WIRINGOP_INC
    if [ $? -ne 0 ]; then
        echo "Error compiling C files"
        echo "Compilation finished with errors!"
        exit 1
    fi
    echo "Compilation finished successfully!"
    exit 0

else
    echo "Error: Undefined platform! OpenPLC can only compile for Windows, Linux and Raspberry Pi environments"
    echo "Compilation finished with errors!"
    exit 1
fi
