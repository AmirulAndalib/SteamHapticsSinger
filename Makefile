steam-haptics-singer : main.cpp midifile/midifile.c rtmidi/RtMidi.cpp
	g++ -D__WINDOWS_MM__ -o steam-haptics-singer main.cpp midifile/midifile.c rtmidi/RtMidi.cpp -lwinmm -lpthread -fpermissive `pkg-config --libs --cflags libusb-1.0`
