#!/usr/bin/perl
# 
# VTR
#
# Adds the missing latch info from the current ABC version 
# to the optimized BLIF file
#
# BLIF Files can potnetially be very large and not fit in the RAM
# The data is not brought into memory because BLIF file 
# Instead, the analysis happens thorugh the disk
#
# Inputs: <ODIN_II_BLIF_FILE> <ABC_BLIF_FILE>

open(odin2File, "<".$ARGV[0]) || die "Error Opening ODIN II File: $!\n";
open(abcFile, "<".$ARGV[1]) || die "Error Opening ABC File: $!\n";

while(($line = <abcFile>)){
	if ($line =~ /^.latch/ ){
		my @tokens = split(/[ \t\n\r]+/, $line);
		$size = @tokens;
		if ($size >= 5){
			print $line;
		} elsif ($size >= 3) {
			#Find missing data from ODIN II BLIF
			$found = 0;
			seek odin2File, 0, 0;
			while(($lineOdn = <odin2File>) && !$found){
				if ($lineOdn =~ /^.latch/ ){
					my @tokensOdn = split(/[ \t\n\r]+/, $lineOdn);
					$sizeOdn = @tokensOdn;
					#print $tokensOdn[2]." ";
					if ($sizeOdn >=5 && $tokens[2] == $tokensOdn[2]){
						#Echo corrected line
						if($sizeOdn == 5){
							print join(" ", (@tokens[0], @tokens[1], @tokens[2], @tokensOdn[3], @tokensOdn[4], @tokensOdn[5]) ), "\n";
						} else {
							print join(" ", (@tokens[0], @tokens[1], @tokens[2], @tokensOdn[3], @tokensOdn[4], @tokensOdn[5]) ), "\n";
						}
						$found = 1;
					}
				}
			}

			if(!$found){
				die "Latch ".$tokens[2]." not found in Odin II BLIF!\n".$line."\n";
				exit -1;
			}
		} else {
			die "Unexpected latch format: ".$size." tokens\n$line\n";
		}
	} else {
		print $line;
	}
}

close(odin2File);
close(abcFile);