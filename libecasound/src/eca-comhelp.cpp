#include "eca-comhelp.h"

const char* ecasound_parameter_help_rep =
"USAGE: (qt)ecasound [options] \n"
"     -c                       set interactive mode \n"
"     -d:debug_level           show debug info \n"
"     -q                       quiet mode, no output \n"
"     --help                   show this help\n"
"     --version                print version info\n"
"     -s[:]file                load chainsetup from 'file' \n"
" --- \n"
"     -b:buffersize            size of sample buffer in samples \n"
"     -m:mixmode               mixmode\n"
"     -n:name                  set chainsetup name\n"
"     -r                       raise runtime prioritary\n"
"     -sr:sample_rate          set internal sample rate\n"
"     -x                       truncate outputs\n"
"     -z:feature               enable feature 'feature'\n"
" --- \n"
"     -t:seconds               processing time in seconds\n"
"     -tl                      enable looping\n"
" --- \n"
"     -a:name1, name2, ...     select/create chains ('all' reserved)\n"
"     -f:type,channels,srate   default file format (for all following inputs/outputs)\n"
"     -i[:]infile              specify a new infile (assigned to active chains)\n"
"     -o[:]outfile             specify a new outfile (assigned to active chains)\n"
"     -y:seconds               set start position for last specified input/output\n"
" --- \n"
"     -pf:preset.eep           insert the first preset from file 'preset.eep'\n"
"     -pn:preset_name          insert a preset 'preset_name'\n"
" --- \n"
"     -ea:params               amplify\n"
"     -eac:params              channel amplify\n"
"     -eaw:params              amplify with clip-control\n"
"     -ec:params               compressor\n"
"     -eca:params              advanced compressor\n"
"     -ef1:params              resonant bandpass filter\n"
"     -ef3:params              resonant lowpass filter\n"
"     -ef4:params              resonant lowpass filter (2nd-order,24dB)\n"
"     -efa:params              allpass filter\n"
"     -efb:params              bandpass filter\n"
"     -efc:params              comb filter\n"
"     -efh:params              highpass filter\n"
"     -efi:params              inverse comb filter\n"
"     -efl:params              lowpass filter\n"
"     -efr:params              bandreject filter\n"
"     -efs:parms               resonator filter\n"
"     -ei:params               pitch shifter\n"
"     -el:name,params          LADSPA-plugin\n"
"     -eli:id_number,params    LADSPA-plugin (unique-id)\n"
"     -enm:params              noise gate\n"
"     -erc:params              copy channel 'source' to 'target'\n"
"     -erm:params              mix all channels to channel 'target' \n"
"     -epp:params              normal pan\n"
"     -etc:params              chorus\n"
"     -etd:params              delay\n"
"     -etm:params              multitap delay\n"
"     -etf:params              fake stereo\n"
"     -etl:params              flanger\n"
"     -etp:params              phaser\n"
"     -etr:params              reverb\n"
"     -ev                      analyze/maximize volume\n"
"     -ezf                     find optimal value for DC-fix\n"
"     -ezx:params              adjust DC \n"
" --- \n"
"     -gc:params               time crop gate\n"
"     -ge:params               threshold gate\n"
" --- \n"
"     -kos:params              sine-oscillator \n"
"     -kf:params               file envelope (generic oscillator) \n"
"     -kl:params               linear envelope (fade-in and fade-out)\n"
"     -kl2:params              two-stage linear envelope\n"
"     -km:params               MIDI-controlled envelope\n"
" --- \n"
"     -kx                      use last specified controller as\n"
"                              controller target\n"
"\n"
"For a more detailed documentation, see ecasound(1) man page.\n"
"Report bugs to <k@eca.cx>.\n";

const char* ecasound_parameter_help(void) {
  return(ecasound_parameter_help_rep);
}
