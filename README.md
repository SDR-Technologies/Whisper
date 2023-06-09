# Whisper

Experimental WhisperC++ plugin for the SDRVM, based on  Whisper.cpp from Georgi Gerganov https://github.com/ggerganov/whisper.cpp.  

This SDRVM/SDR4Space plugin provides an interface to process demodulated audio by Whisper :-) 

Theory of operations :  

* You have audio samples... look at other examples or VM documentation
* you send the audio samples to the plugin that runs in the background
* The decoded fragments are sent into a MailBox.

** WARNING ** Make sure you have the latest SDRVM binary... older versions could not read mono WAV files.

Example :  

``` javascript
// Load the plugin
var ok = loadPluginLib('/opt/vmbase/extensions/', 'libwhisper');
if( ok == false ) {
    print('cannot load lib, stop.');
    exit();
}


var ok = ImportObject('libwhisper', 'Whisper');
if( ok == false ) {
    print('Cannot load object');
    exit();
}

// Create the mailbox to read the decoded fragments
MBoxCreate('whisper');

// Configure the plugin
var w = new Whisper();
if( w.configure('/opt/vmbase/models/ggml-medium.bin', 'en', 'whisper') != true ) {
    print('cannot configure');
    exit();
}

// Load JFK
var IQ = new IQData();
IQ.loadFromFile('jfk_stereo.wav'); 
IQ.dump();

// Process file
w.processData( IQ );

for( ; ; ) {
    var msg = MBoxPopWait('whisper', 1000);
    if( msg.msg_valid ) {
    	var text = msg.msg_payload ;
    	print( '> ' + text );
    	w.processData( IQ );
    }
}

```