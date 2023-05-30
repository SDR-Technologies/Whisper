
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

MBoxCreate('whisper');

// Creer l'objet Whisper et le configurer
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
