#!/usr/bin/perl
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#

$ROOT="..";

if($ARGV[0] eq "-r") {
    $ROOT=$ARGV[1];
    shift @ARGV;
    shift @ARGV;    
}

my $verbose;
if($ARGV[0] eq "-v") {
    $verbose =1;
    shift @ARGV;
}

my $firmdir="$ROOT/firmware";

my $wpslist=$ARGV[0];

my $target = $ARGV[1];
my $cppdef = $target;


if(!$wpslist) {
    print "Usage: wpsbuilds.pl <WPSLIST> <target>\n",
    "Run this script in the root of the target build, and it will put all the\n",
    "stuff in .rockbox/wps/\n";
    exit;
}

sub getlcdsizes {
    my ($remote) = @_;

    open(GCC, ">gcctemp");
    if($remote) {
        # Get the remote LCD screen size
    print GCC <<STOP
\#include "config.h"
#ifdef HAVE_REMOTE_LCD
Height: LCD_REMOTE_HEIGHT
Width: LCD_REMOTE_WIDTH
#endif
STOP
;
    }
    else {
    print GCC <<STOP
\#include "config.h"
Height: LCD_HEIGHT
Width: LCD_WIDTH
STOP
;
}
    close(gcc);

    my $c="cat gcctemp | gcc $cppdef -I. -I$firmdir/export -E -P -";

    #print "CMD $c\n";

    open(GETSIZE, "$c|");

    my ($height, $width);
    while(<GETSIZE>) {
        if($_ =~ /^Height: (\d*)/) {
            $height = $1;
        }
        elsif($_ =~ /^Width: (\d*)/) {
            $width = $1;
        }
        if($height && $width) {
            last;
        }
    }
    close(GETSIZE);
    unlink("gcctemp");

    return ($height, $width);
}

sub mkdirs {        
    my $wpsdir = $wps;
    $wpsdir =~ s/\.(r|)wps//;
    mkdir ".rockbox/wps", 0777;
    mkdir ".rockbox/theme", 0777;

    if( -d ".rockbox/wps/$wpsdir") {
        #print STDERR "wpsbuild warning: directory wps/$wpsdir already exists!\n";
    }
    else {
        mkdir ".rockbox/wps/$wpsdir", 0777;
    }
}

sub copywps {
    # we assume that we copy the WPS files from the same dir the WPSLIST
    # file is located in
    my $dir;

    if($wpslist =~ /(.*)WPSLIST/) {
        $dir = $1;
        my $wpsdir = $wps;
        $wpsdir =~ s/\.(r|)wps//;
        system("cp $dir/$wps .rockbox/wps/");

        if (-e "$dir/$wpsdir") {
           system("cp $dir/$wpsdir/*.bmp .rockbox/wps/$wpsdir/");
        }
    }
    else {
        print STDERR "beep, no dir to copy WPS from!\n";
    }
}

sub buildcfg {
    my $cfg = $wps;
    my @out;

    $cfg =~ s/\.(r|)wps/.cfg/;

    push @out, <<MOO
\#
\# $cfg generated by wpsbuild.pl
\# $wps is made by $author
\#
wps: /.rockbox/wps/$wps
MOO
;
    if($font) {
        push @out, "font: /.rockbox/fonts/$font\n";
    }
    if($statusbar) {
        push @out, "statusbar: $statusbar\n";
    }
    if($rwps && !$isrwps) {
        push @out, "rwps: /.rockbox/wps/$rwps\n";
    }

    if(-f ".rockbox/wps/$cfg") {
        print STDERR "wpsbuild warning: wps/$cfg already exists!\n";
    }
    else {
        open(CFG, ">.rockbox/theme/$cfg");
        print CFG @out;
        close(CFG);
    }
}

# Get the LCD sizes first
my ($main_height, $main_width) = getlcdsizes();
my ($remote_height, $remote_width) = getlcdsizes(1);

open(WPS, "<$wpslist");
while(<WPS>) {
    my $l = $_;
    if($l =~ /^ *\#/) {
        # skip comment
        next;
    }
    if($l =~ /^ *<(r|)wps>/i) {
        $isrwps = $1;
        $within = 1;
        next;
    }
    if($within) {
        if($l =~ /^ *<\/${isrwps}wps>/i) {
            # Get the required width and height
            my ($rheight, $rwidth);
            if($isrwps) {
                ($rheight, $rwidth) = ($remote_height, $remote_width);
            }
            else {
                ($rheight, $rwidth) = ($main_height, $main_width);
            }

            if(!$rheight || !$rwidth) {
                printf STDERR "wpsbuild notice: No %sLCD size, skipping $wps\n",
                $isrwps?"remote ":"";
                next;
            }

            #print "LCD: $wps wants $height x $width\n";
            #print "LCD: is $rheight x $rwidth\n";

            if(($height <= $rheight) && ($width <= $rwidth)) {
                #
                # The target model has an LCD that is suitable for this
                # WPS
                #
                #print "Size requirement is fine!\n";

                mkdirs();
                if(!$isrwps) {
                    # We only make .cfg files for <wps> sections:
                    buildcfg();
                }
                copywps();
            }
            else {
                #print "Skip $wps due to size restraints\n";
            }
            $within = 0;

            undef $wps, $rwps, $width, $height, $font, $statusbar, $author;
        }
        elsif($l =~ /^Name: (.*)/i) {
            # Note that in the case this is within <rwps>, $wps will contain the
            # name of the rwps. Use $isrwps to figure out what type it is.
            $wps = $1;
        }
        elsif($l =~ /^RWPS: (.*)/i) {
            $rwps = $1;
        }
        elsif($l =~ /^Author: (.*)/i) {
            $author = $1;
        }
        elsif($l =~ /^Width: (.*)/i) {
            $width = $1;
        }
        elsif($l =~ /^Height: (.*)/i) {
            $height = $1;
        }
        elsif($l =~ /^Font: (.*)/i) {
            $font = $1;
        }
        elsif($l =~ /^Statusbar: (.*)/i) {
            $statusbar = $1;
        }
    }
}

close(WPS)
