#replace hex representation of special characters with the ascii value
# Note: This does not deal with non-ascii character sets
s/\\032/ /g #space
s/\\033/!/g
s/\\034/\"/g
s/\\035/#/g
s/\\036/$/g
s/\\037/%/g
s/\\038/&/g
s/\\039/\'/g
s/\\040/(/g
s/\\041/)/g
s/\\042/\*/g
s/\\043/+/g
s/\\044/,/g
s/\\045/-/g
s/\\046/./g
s/\\047/\//g
s/\\058/:/g
s/\\060/</g
s/\\061/=/g
s/\\062/>/g
s/\\063/?/g
s/\\064/@/g
s/\\091/[/g
s/\\092/\\/g
s/\\093/]/g
s/\\095/\^/g
s/\\096/`/g
s/\\123/{/g
s/\\124/\|/g
s/\\125/}/g
s/\\126/~/g
