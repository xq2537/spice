python ..\..\spice-common\spice_codegen.py -d -c  -i common.h -i messages.h --prefix 1 --ptrsize 8 ..\..\spice-common\spice1.proto ..\generated_demarshallers1.cpp
python ..\..\spice-common\spice_codegen.py --generate-marshallers -P --include "common.h" --include messages.h  --include client_marshallers.h --client --prefix 1 --ptrsize 8 ..\..\spice-common\spice1.proto ..\generated_marshallers1.cpp
