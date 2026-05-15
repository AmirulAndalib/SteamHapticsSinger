#include <iostream>
#include <chrono>

#include <stdint-gcc.h>
#include <unistd.h>
#include <stdint.h>

#include <signal.h>
#include <stdio.h>

#include <libusb-1.0/libusb.h>
#include "midifile/midifile.h"

#include <cstdlib>
#include "rtmidi/RtMidi.h"

#define STEAM_CONTROLLER_MAGIC_PERIOD_RATIO 495483.0
#define CHANNEL_COUNT					   4
#define DEFAULT_INTERVAL_USEC			   10000

#define DURATION_MAX		-1
#define NOTE_STOP		   -1

#define DEFAULT_GAIN 127

using namespace std;

double midiFrequency[128]  = {0, 8.66196, 9.17702, 9.72272, 10.3009, 10.9134, 11.5623, 12.2499, 12.9783, 13.75, 14.5676, 15.4339, 16.3516, 17.3239, 18.354, 19.4454, 20.6017, 21.8268, 23.1247, 24.4997, 25.9565, 27.5, 29.1352, 30.8677, 32.7032, 34.6478, 36.7081, 38.8909, 41.2034, 43.6535, 46.2493, 48.9994, 51.9131, 55, 58.2705, 61.7354, 65.4064, 69.2957, 73.4162, 77.7817, 82.4069, 87.3071, 92.4986, 97.9989, 103.826, 110, 116.541, 123.471, 130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220, 233.082, 246.942, 261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440, 466.164, 493.883, 523.251, 554.365, 587.33, 622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880, 932.328, 987.767, 1046.5, 1108.73, 1174.66, 1244.51, 1318.51, 1396.91, 1479.98, 1567.98, 1661.22, 1760, 1864.66, 1975.53, 2093, 2217.46, 2349.32, 2489.02, 2637.02, 2793.83, 2959.96, 3135.96, 3322.44, 3520, 3729.31, 3951.07, 4186.01, 4434.92, 4698.64, 4978.03, 5274.04, 5587.65, 5919.91, 6271.93, 6644.88, 7040, 7458.62, 7902.13, 8372.02, 8869.84, 9397.27, 9956.06, 10548.1, 11175.3, 11839.8, 12543.9};


struct ParamsStruct{
	const char* midiSong;
	unsigned int intervalUSec;
	int libusbDebugLevel;
	bool repeatSong;
	signed char leftGain;
	signed char rightGain;
};

enum class ControllerType {
	None,
	Original,
	Triton,
	Jupiter,
	Galileo
};

struct SteamControllerInfos{
	libusb_device_handle* dev_handle;
	int interfaceNum;

	ControllerType type = ControllerType::None;
	signed char leftGain;
	signed char rightGain;
	int endPoint;
};

SteamControllerInfos steamController1;
bool isDeck = false;

int periodRatio = 515000;

bool SteamController_Open(SteamControllerInfos* controller){
	if(!controller)
		return false;

	libusb_device_handle* dev_handle;
	//Open Steam Controller device
	if((dev_handle = libusb_open_device_with_vid_pid(NULL, 0x28DE, 0x1102)) != NULL){ // Wired Steam Controller (2015)
		cout<<"Found wired Steam Controller (2015)"<<endl;
		controller->dev_handle = dev_handle;
		controller->interfaceNum = 2;
		controller->type = ControllerType::Original;
	}
	else if((dev_handle = libusb_open_device_with_vid_pid(NULL, 0x28DE, 0x1142)) != NULL){ // Steam Controller (2015) dongle //TODO: FIX
		cout<<"Found Steam Dongle, will attempt to use the first Steam Controller (2015)"<<endl;
		controller->dev_handle = dev_handle;
		controller->interfaceNum = 1;
		controller->type = ControllerType::Original;
	} 
	else if((dev_handle = libusb_open_device_with_vid_pid(NULL, 0x28DE, 0x1302)) != NULL){ // Steam Controller (2026)
		cout<<"Found wired Steam Controller (2026)"<<endl;
		controller->dev_handle = dev_handle;
		controller->interfaceNum = 0;
		controller->type = ControllerType::Triton;
		controller->endPoint = 0x01;
	}
	else if((dev_handle = libusb_open_device_with_vid_pid(NULL, 0x28DE, 0x1304)) != NULL){ // Steam Puck
		cout<<"Found Steam Puck, will attempt to use the first Steam Controller (2026)"<<endl;
		controller->dev_handle = dev_handle;
		controller->interfaceNum = 2;
		controller->type = ControllerType::Triton;
		controller->endPoint = 0x02;
		//Workaround for controller disconnecting after puck reconnects on Windows
		#ifdef _WIN32
		cout << "Reconnect controller and press Enter...";
    	cin.get(); 
		#endif
	}
	else if((dev_handle = libusb_open_device_with_vid_pid(NULL, 0x28DE, 0x1205)) != NULL){ // Steam Deck
		cout<<"Found Steam Deck"<<endl;
		controller->dev_handle = dev_handle;
		controller->interfaceNum = 2;
		controller->type = ControllerType::Jupiter;
	}
	else{
		cout<<"No device found"<<endl;
		return false;
	}

	//On Linux, automatically detach and reattach kernel module
	libusb_set_auto_detach_kernel_driver(controller->dev_handle,1);
	
	return true;
}

bool SteamController_Claim(SteamControllerInfos* controller){
	//Claim the USB interface controlling the haptic actuators
	int r = libusb_claim_interface(controller->dev_handle,controller->interfaceNum);
	if(r < 0) {
		cout<<"Interface claim Error "<<r<<endl;
		std::cin.ignore();
		libusb_close(controller->dev_handle);
		return false;
	}

	return true;
}

void SteamController_Close(SteamControllerInfos* controller){
	int r = libusb_release_interface(controller->dev_handle,controller->interfaceNum);
	if(r < 0) {
		cout<<"Interface release Error "<<r<<endl;
		std::cin.ignore();
		return;
	}
}

//Steam Haptics Playblack
int SteamHaptics_PlayNote(SteamControllerInfos* controller, int channel, int note, int velocity){
	if (channel > 1 && controller->type != ControllerType::Triton) return 1;
	unsigned char dataBlob[16] = {0};
	
	double frequency = midiFrequency[note];
	uint16_t duration = (note == NOTE_STOP) ? 0x0000 : 0x7fff;

	int r;

	double period;
	uint16_t periodCommand;
	uint16_t repeatCommand;
	uint16_t gainCommand;
	
	//cout << velocity << endl;

	switch(controller->type) {
	case ControllerType::Original: //Steam Controller (2015) Playback

		period = 1.0 / frequency;
		periodCommand = period * STEAM_CONTROLLER_MAGIC_PERIOD_RATIO; //Reminder to check if the Steam Controller tuning lines up with the Deck.
		repeatCommand = (note == NOTE_STOP) ? 0x0000 : 0x7fff;
		gainCommand = (velocity * 65535) / 127;

		dataBlob[0] = 0x8F;
		dataBlob[2] = channel;
		dataBlob[3] = periodCommand % 0xFF;
		dataBlob[4] = periodCommand / 0xFF;
		dataBlob[5] = periodCommand % 0xFF;
		dataBlob[6] = periodCommand / 0xFF;
		dataBlob[7] = repeatCommand % 0xFF;
		dataBlob[8] = repeatCommand / 0xFF;
		dataBlob[9] = gainCommand % 0xFF; //Doesn't work
		dataBlob[10]= gainCommand / 0xFF;
		r = libusb_control_transfer(controller->dev_handle,0x21,9,0x0300,controller->interfaceNum,dataBlob,16,1000);
		if(r < 0) {
			cout<<"Command Error "<<r<< endl;
			exit(0);
		}
		break;

	case ControllerType::Triton: //Steam Controller (2026) Playback

		if (note == NOTE_STOP) {
			//This prevents the controller from rebooting when using back motors
			dataBlob[0] = 0x81;
			dataBlob[1] = (channel < 2) ? channel : !(channel-2)+3;
			//cout << ((channel < 2) ? channel : !channel+1) << endl;
		} else {
			dataBlob[0] = 0x83;
			dataBlob[1] = (channel < 2) ? !channel : !(channel-2)+3;
			dataBlob[2] = (velocity * 255) / 127 - 128;
			dataBlob[3] = (int)frequency % 0xFF;
			dataBlob[4] = (int)frequency / 0xFF;
			dataBlob[5] = 0xFF;
			dataBlob[6] = 0x7F;
		}
		
		r = libusb_interrupt_transfer(controller->dev_handle,controller->endPoint,dataBlob,16,NULL,1000);
		if(r < 0) {
			cout<<"Command Error "<<r<< endl;
			exit(0);
		}
		break;

	case ControllerType::Jupiter: //Steam Deck Playback
	
		dataBlob[0] = 0xEA;
		dataBlob[2] = !channel; //Swap haptics to match 2015
		dataBlob[3] = 0x03; 
		dataBlob[5] = (velocity * 255) / 127 - 128;
		dataBlob[6] = (int)frequency % 0xFF;
		dataBlob[7] = (int)frequency / 0xFF;
		dataBlob[8] = duration % 0xFF;
		dataBlob[9] = duration / 0xFF;
		r = libusb_control_transfer(controller->dev_handle,0x21,9,0x0300,2,dataBlob,16,1000);
		if(r < 0) {
			cout<<"Command Error "<<r<< endl;
			exit(0);
		}
		break;
	
	}

	return 0;
}

float timeElapsedSince(std::chrono::steady_clock::time_point tOrigin){
	using namespace std::chrono;
	steady_clock::time_point tNow = steady_clock::now();
	duration<double> time_span = duration_cast<duration<double>>(tNow - tOrigin);
	return time_span.count();
}


void displayPlayedNotes(int channel, int8_t note){
	static int8_t notePerChannel[CHANNEL_COUNT] = {NOTE_STOP,NOTE_STOP,NOTE_STOP,NOTE_STOP};
	const char* textPerChannel[CHANNEL_COUNT] = {"LEFT haptic : ",", RIGHT haptic : ","LEFT rumble : ",", RIGHT rumble : "};
	const char* noteBaseNameArray[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};

	if(channel >= CHANNEL_COUNT)
		return;

	notePerChannel[CHANNEL_COUNT-1-channel] = note;

	for(int i = 0 ; i < CHANNEL_COUNT ; i++){
		cout << textPerChannel[i];

		//Write empty string
		if(notePerChannel[i] == NOTE_STOP){
			cout << "OFF ";
		}
		else{
			//Write note name
			cout << noteBaseNameArray[notePerChannel[i]%12];
			int octave = (notePerChannel[i]/12)-1;
			cout << octave;
			if(octave >= 0 ){
				cout << " ";
			}
		}
	}

	cout << "\r" ;
	cout.flush();
}

void playSong(SteamControllerInfos* controller,const ParamsStruct params){

	MidiFile_t midifile;

	//Open Midi File
	midifile = MidiFile_load(params.midiSong);

	if(midifile == NULL){
		cout << "Unable to open MIDI file!" << params.midiSong << endl;
		return;
	}

	//Check if file contains at least one midi event
	if(MidiFile_getFirstEvent(midifile) == NULL){
		cout << "MIDI file is empty!" << endl;
		return;
	}

	//Waiting for user to press enter; YOURE WRONG, SULFURIC ACID!
	cout << "Starting playback of " << params.midiSong  << "..." << endl;
	sleep(1);

	//This will contains the previous events accepted for each channel
	MidiFileEvent_t acceptedEventPerChannel[CHANNEL_COUNT] = {0};

	//Get current time point, will be used to know elapsed time
	std::chrono::steady_clock::time_point tOrigin = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point tRestart = std::chrono::steady_clock::now();

	//Iterate through events
	MidiFileEvent_t currentEvent = MidiFile_getFirstEvent(midifile);
	
	while(currentEvent != NULL){
		usleep(params.intervalUSec);

		//This will contains the events to play
		MidiFileEvent_t eventsToPlay[CHANNEL_COUNT] = {NULL};

		//We now need to play all events with tick < currentTime
		long currentTick = MidiFile_getTickFromTime(midifile,timeElapsedSince(tOrigin));

		//Iterate through all events until the current time, and selecte potential events to play
		for( ; currentEvent != NULL && MidiFileEvent_getTick(currentEvent) < currentTick ; currentEvent = MidiFileEvent_getNextEventInFile(currentEvent)){

			//Only process note start events or note end events matching previous event
			if (!MidiFileEvent_isNoteStartEvent(currentEvent) && !MidiFileEvent_isNoteEndEvent(currentEvent)) continue;

			//Get channel event
			int eventChannel = MidiFileVoiceEvent_getChannel(currentEvent);

			//If channel is other than 0 or 1, skip this event, we cannot play it with only 1 steam controller
			if(eventChannel < 0 || !(eventChannel < CHANNEL_COUNT)) continue;

			//If event is note off and does not match previous played event, skip it
			if(MidiFileEvent_isNoteEndEvent(currentEvent)){
				MidiFileEvent_t previousEvent = acceptedEventPerChannel[eventChannel];

				//Skip if current event is not ending previous event,
				// or if they share the same tick ( end event after start evetn on same tick )
				if(MidiFileNoteStartEvent_getNote(previousEvent) != MidiFileNoteEndEvent_getNote(currentEvent)
				||(MidiFileEvent_getTick(currentEvent) == MidiFileEvent_getTick(previousEvent)))
					continue;
			}

			//If we arrive here, this event is accepted
			eventsToPlay[eventChannel] = currentEvent;
			acceptedEventPerChannel[eventChannel]=currentEvent;
		}

		//Now play the last events found
		for(int currentChannel = 0 ; currentChannel < CHANNEL_COUNT ; currentChannel++){
			MidiFileEvent_t selectedEvent = eventsToPlay[currentChannel];

			//If no note event available on the channel, skip it
			if(!MidiFileEvent_isNoteStartEvent(selectedEvent) && !MidiFileEvent_isNoteEndEvent(selectedEvent)) continue;

			//Set note event
			int8_t eventNote = NOTE_STOP;
			if(MidiFileEvent_isNoteStartEvent(selectedEvent)){
				eventNote = MidiFileNoteStartEvent_getNote(selectedEvent);
			}

			//Play notes
			if (isDeck) {
				//SteamDeck_PlayNote(controller,!currentChannel,eventNote,100); //Why is currentChannel like this? The Deck reversed the trackpad order, and this is to accommodate for that. Plan to change when channel selecting is implemented.
			} else {
				//SteamController_PlayNote(controller,currentChannel,eventNote,0);
			}
			displayPlayedNotes(currentChannel,eventNote);
		}
	}

	cout <<endl<< "Playback completed " << endl;
}

void midiCallback(double deltatime, std::vector<unsigned char>* message, void* userData) { //// TEMPORARY!!!!!! This will need to be removed, redone, and integrated into the realTime fuctnion.
    if (message->size() >= 3) {
        int messageType = message->at(0) & 0xf0; // Extract the message type (Note On, Note Off, etc.)
		int channel = message->at(0) & 0x0f; 
        int note = message->at(1);
        int velocity = message->at(2);
			
		switch (messageType) {
			case 0x90: //Note On Message
				if (velocity != 0) {
					SteamHaptics_PlayNote(&steamController1,channel,NOTE_STOP,0);
					SteamHaptics_PlayNote(&steamController1,channel,note,velocity);
					displayPlayedNotes(channel, note);
				} else {
					SteamHaptics_PlayNote(&steamController1,channel,NOTE_STOP,0);
					displayPlayedNotes(channel, NOTE_STOP);
				}
				break;
			case 0x80: //Note Off Message
				SteamHaptics_PlayNote(&steamController1,channel,NOTE_STOP,0);
				displayPlayedNotes(channel, NOTE_STOP);
				break;
		}
	}
}

void realTime(SteamControllerInfos* controller,const ParamsStruct params){
	//Set up RTMIDI
	RtMidiIn *midiin = 0;
  	std::vector<unsigned char> message;

	//RtMidiIn constructor
	try {
    	midiin = new RtMidiIn();
  	}
  	catch ( RtMidiError &error ) {
    	error.printMessage();
    	exit( EXIT_FAILURE );
  	}

	//Check available ports vs. specified.
	unsigned int port = 0;
	unsigned int nPorts = midiin->getPortCount();
	if ( port >= nPorts ) {
		delete midiin;
		std::cout << "Invalid port specifier!\n";
		exit( 0 );
	}

	//Open port
	try {
    	midiin->openPort( port );
  	}
  	catch ( RtMidiError &error ) {
    	error.printMessage();
    	goto cleanup;
  	}

	midiin->setCallback(midiCallback);

    std::cout << "Listening to MIDI input on port: " << midiin->getPortName( port ) << std::endl;

    // Enter a loop to keep the program running and capture MIDI input
    char input;
    std::cout << "Press Enter to exit." << std::endl;
    std::cin.get(input);

	/*done = false;
	while ( !done ) {
    	midiin->getMessage( &message );
		if (message.size() >= 3) {
        	int messageType = message.at(0) & 0xf0; // Extract the message type (Note On, Note Off, etc.)
			int channel = message.at(0) & 0x0f; 
        	int note = message.at(1);
        	int velocity = message.at(2);
			if(!(channel < CHANNEL_COUNT)) continue;
			if (messageType == 0x90) { // Note On message
            	std::cout << "Note On: " << note << ", Velocity: " << velocity << ", Channel: " << channel << std::endl;
				SteamDeck_PlayNote(controller,!channel,note);
        	} else if (messageType == 0x80) { // Note Off message
            	std::cout << "Note Off: " << note << ", Velocity: " << velocity << ", Channel: " << channel << std::endl;
				SteamDeck_PlayNote(controller,!channel,NOTE_STOP);
			}
        }
		}*/

	cleanup:
  		delete midiin;
	}






bool parseArguments(int argc, char** argv, ParamsStruct* params){
	int c;
	while ( (c = getopt(argc, argv, "k:j:c:l:i:r")) != -1) {
		unsigned long int value;
		switch(c){
		case 'k':
			value = strtoul(optarg,NULL,10);
			if(value <= 255 && value > 0){
				params->rightGain = value;
			}
			break;
		case 'j':
			value = strtoul(optarg,NULL,10);
			if(value <= 255 && value > 0){
				params->leftGain = value;
			}
			break;
		case 'l':
			value = strtoul(optarg,NULL,10);
			if(value >= LIBUSB_LOG_LEVEL_NONE && value <= LIBUSB_LOG_LEVEL_DEBUG){
				params->libusbDebugLevel = value;
			}
			break;
		case 'i':
			value = strtoul(optarg,NULL,10);
			if(value <= 1000000 && value > 0){
				params->intervalUSec = value;
			}
			break;
		case 'r':
			params->repeatSong = true;
			break;
		case '?':
			return false;
			break;
		default:
			break;
		}
	}
	/*if(optind == argc-1 ){
		params->midiSong = argv[optind];
		return true;
	}
	else{
		return false;
	}*/
	return true;
}


void abortPlaying(int){
	for(int i = 0 ; i < CHANNEL_COUNT ; i++){
		//if (isDeck) {
		//	SteamDeck_PlayNote(&steamController1,i,NOTE_STOP,0);
		//} else {
		//	SteamController_PlayNote(&steamController1,i,NOTE_STOP,0);
		//}
		SteamHaptics_PlayNote(&steamController1,i,NOTE_STOP,0);
	}

	SteamController_Close(&steamController1);

	cout << endl<< "Aborted " << endl;
	cout.flush();
	exit(1);
}

int main(int argc, char** argv)
{
	cout <<"Steam Haptics Singer by Crazy, Steam Controller Singer by Pila"<<endl;

	ParamsStruct params;
	params.intervalUSec = DEFAULT_INTERVAL_USEC;
	params.libusbDebugLevel = LIBUSB_LOG_LEVEL_NONE;
	params.repeatSong = false;
	params.midiSong = "\0";
	params.leftGain = DEFAULT_GAIN;
	params.rightGain = DEFAULT_GAIN;


	//Parse arguments
	if(!parseArguments(argc, argv, &params)){
		cout << "Usage: steam-haptics-singer [-l DEBUG_LEVEL] [-i INTERVAL]" << endl;
		return 1;
	}


	//Initializing LIBUSB
	libusb_context * ctx = NULL;
	int r = libusb_init(&ctx);
	libusb_set_option(ctx, LIBUSB_OPTION_USE_USBDK);
	if(r < 0) {
		cout<<"LIBUSB Init Error "<<r<<endl;
		std::cin.ignore();
		return 1;
	}

	libusb_set_debug(NULL, params.libusbDebugLevel);

	//Gaining access to Steam Controller
	if(!SteamController_Open(&steamController1)){
		return 1;
	}
	if(!SteamController_Claim(&steamController1)){
		return 1;
	}

	//Set mecanism to stop playing when closing process
	signal(SIGINT, abortPlaying);

	//Playing song
	/*do{
		playSong(&steamController1,params);
	}while(params.repeatSong);*/

	//Real time MIDI
	realTime(&steamController1,params);

	//Releasing access to Steam Controller
	SteamController_Close(&steamController1);
	
	libusb_close((&steamController1)->dev_handle);

	libusb_exit(NULL);

	return 0;
}
