python ..\..\spice-common\spice_codegen.py -d -c  -i common.h -i messages.h ..\..\spice-common\spice.proto ..\generated_demarshallers.cpp
python ..\..\spice-common\spice_codegen.py --generate-marshallers -P --include "common.h" --include messages.h --include client_marshallers.h --client ..\..\spice-common\spice.proto ..\generated_marshallers.cpp
