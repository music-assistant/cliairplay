# cliairplay tools

Tools to assist with development and testing of cliap

## tool.sh

This is a shell script to facilitate easy running of the cliap binary. It requires that the `avahi-browse`
tool is installed. The script will start `cliap` for the nominated AirPlay device and expect raw PCM audio
to be supplied on stdin.

It makes use of the tool.sed file to assist with decoding the `avahi-browse` output.