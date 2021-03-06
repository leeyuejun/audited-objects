#!/usr/bin/perl

use strict;

use Cwd qw(getcwd realpath);
use File::Basename;
use File::Spec;
use File::Temp qw(tempfile);
use Getopt::Long qw(:config no_ignore_case);
use Pod::Usage;

use FindBin;
use lib "$FindBin::RealBin/../lib/perl5";
use lib "$FindBin::Bin/../lib/perl5";
use AO::PathAction;
use AO::CommandAction;
use AO::System qw($prog $verbosity);

pod2usage(1) if !@ARGV;

my %opt;
GetOptions(\%opt, qw(help absolute base=s members-only quiet short Cmd=s Intpaths Srcpaths Tgtpaths));

$opt{Intpaths} = $opt{Srcpaths} = $opt{Tgtpaths} = 1
    unless $opt{Intpaths} || $opt{Srcpaths} || $opt{Tgtpaths} || $opt{Cmd};

pod2usage(1) if $opt{help};

chomp(my($ProjName, $BaseDir) = qx(ao -q property Project.Name Base.Dir));
$BaseDir = Cwd::realpath($opt{base}) if $opt{base};
$BaseDir =~ s%/%\\%g if MSWIN;

@ARGV = ("$ProjName.ao") if (!@ARGV || $ARGV[0] eq '-') && -t STDIN;

my($ca, @Actions, %FileTargets, %DirTargets, %DirLinkTargets);
while (<>) {
    chomp;
    if (m%^\d%) {
	next if $ca;
	$ca = CommandAction->new($_);
	push(@Actions, $ca);
    } elsif (m%^[A-Z]%) {
	my $pa = PathAction->new($_, $BaseDir);

	my $path = $pa->get_path;

	if ($opt{'existing-only'} && ! -e $path) {
	    # Optionally ignore files which did not survive the build.
	} elsif ($opt{'members-only'} && !$pa->is_member) {
	    # Optionally ignore nonmember files.
	} elsif ($pa->is_target) {
	    $ca->add_target($pa);
	    if ($pa->is_dir) {
		if (!exists($DirTargets{$path})) {
		    $DirTargets{$path} = $pa;
		    $DirLinkTargets{$path} = $pa if $pa->is_symlink;
		}
	    }
	} else {
	    $ca->add_prereq($pa) unless $pa->is_unlink;
	}
    } else {
	$ca = undef;
    }
}

if (my $re = $opt{Cmd}) {
    for my $ca (@Actions) {
	my @found = ();
	for my $prq ($ca->get_prereqs) {
	    next if $opt{Tgtpaths};
	    my $path = $prq->get_path;
	    $path =~ s%^$BaseDir[/\\]*%% if !$opt{absolute};
	    next unless $path =~ m%$re%;
	    push(@found, "< $path");
	}
	for my $tgt ($ca->get_targets) {
	    next if $opt{Srcpaths};
	    my $path = $tgt->get_path;
	    $path =~ s%^$BaseDir[/\\]*%% if !$opt{absolute};
	    next unless $path =~ m%$re%;
	    push(@found, "> $path");
	}
	if (@found) {
	    printf "+ [%s] %s\n", $ca->get_rwd, $ca->get_line;
	    if (! $opt{short}) {
		print "$_\n" for @found;
	    }
	}
    }
    exit(0);
}

sub output {
    my $pfx = shift;
    my @list = @_;
    for (@list) {
	s%^$BaseDir[/\\]*%% if !$opt{absolute};
	print "$pfx " unless $opt{quiet};
	print "$_\n";
    }
}

my(%tgtpaths, %srcpaths, %intpaths);

for my $ca (@Actions) {
    for my $tgt ($ca->get_targets) {
	my $path = $tgt->get_path;
	$tgtpaths{$path}++;
    }
}

if ($opt{Srcpaths} || $opt{Intpaths}) {
    for my $ca (@Actions) {
	for my $prq ($ca->get_prereqs) {
	    my $path = $prq->get_path;
	    $srcpaths{$path}++ if !exists $tgtpaths{$path};
	    if (exists $tgtpaths{$path}) {
		$intpaths{$path}++;
		delete $tgtpaths{$path} if $opt{Intpaths};
	    }
	}
    }
}

output('<', sort keys %srcpaths) if $opt{Srcpaths};
output('=', sort keys %intpaths) if $opt{Intpaths};
output('>', sort keys %tgtpaths) if $opt{Tgtpaths};

__END__

=head1 NAME

aoquery - Generic script for pulling data from a raw AO audit file.

=head1 OPTIONS

   -help		Print this message and exit
   -absolute		Show pathnames as absolute
   -base dir		Specify the root dir (default: cwd)
   -members-only	Ignore files which are not project members
   -quiet		Suppress verbosity
   -short		Request short-form output
   -Cmd regex		Show commands associated with paths matching "regex"
   -Srcpaths		Show all "src" (only opened for read) paths
   -Intpaths		Show all "intermediate" paths (written and read)
   -Tgtpaths		Show all "tgt" (ever opened for write) paths

=head2 NOTES

    All flags may be abbreviated to their shortest unique name.
    Input may be read from standard input or a named audit-file

=head2 EXAMPLES

    aoquery -Src < audit-file
    aoquery -C 'foo\.o' audit-file

=head1 DESCRIPTION

This script will read in an audit file and print out aggregated data,
such as the list of source (pre-existing) files, the list of generated
files, an ordered list of commands run, etc.

=head1 COPYRIGHT

Copyright (c) 2010-2011 David Boyce.  All rights reserved.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
