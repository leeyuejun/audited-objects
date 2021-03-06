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
GetOptions(\%opt, qw(base=s BaseVar=s dbg existing-only full
    NOGNU help ext=s members-only output-file|MF=s pdump|PDUMP
    prqre=s Parallel vars=s));

pod2usage(1) if $opt{help};

$opt{full} = 1 if $opt{pdump} || $opt{vars};
my $iext = $opt{full} ? undef : ($opt{'ext'} || 'd');

my $prqre = qr/$opt{prqre}/ if $opt{prqre};

# The dash shell does not implement -n or -v correctly.
my $broken_sh = 0;
if (-x '/bin/sh' && -x '/bin/dash') {
    if ((stat '/bin/sh')[1] == (stat '/bin/dash')[1]) {
	$broken_sh = 1;
	undef $ENV{BASH_ENV};
    }
}

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
# TODO Bug here - $pa cannot currently be both a dir and a symlink.
# We need a way to solve this, preferably without depending on -d.
# Maybe by assuming dirs will only be created to hold files, and thus
# check whether there are other file or dir names beginning with the
# resolved path?
		}
	    }
	} else {
	    next if $prqre && $path !~ m%$prqre%;
	    $ca->add_prereq($pa) unless $pa->is_unlink;
	}
    } else {
	$ca = undef;
    }
}

###########################################################################
# To this point we've been reading in data and parsing it into an internal
# representation, while from here on we're engaged in deriving a makefile
# from that data. Therefore, modifying this script to produce another
# format _should_ require only changes below this point (leaving aside
# flags etc).
###########################################################################

{
    package MakeVar;

    sub new {
	my $proto = shift;
	my $self = {};
	bless $self, $proto;
	$self->{EXPLICIT} = 1;
	if (my $line = shift) {
	    my($name, $eq, $val) = $line =~ m%^(.*?)\s+([^=]*=)\s*(.*?)\s*$%;
	    $self->assign($name, $eq, $val);
	}
	return $self;
    }

    sub assign {
	my $self = shift;
	my($name, $eq, $val) = @_;
	$self->{NAME} = $name;
	$self->{EQ} = $eq;
	$self->{VALUE} = $val;
	return $self;
    }

    # Prints a one-line variable definition in a neatly formatted way.
    sub _format {
	my $self = shift;
	my $eq = shift;
	my $ending = shift || '';
	return sprintf "%-*s %s %s$ending\n",
	    25 - length($eq), $self->get_name, $eq, $self->get_value || '';
    }

    sub format {
	my $self = shift;
	my $eq = $self->{EQ} || ($opt{NOGNU} ? '=' : ':=');
	return $self->_format($eq, @_);
    }

    sub format_ifndef {
	my $self = shift;
	my $eq = ($opt{NOGNU} ? '=' : '?=');
	return $self->_format($eq, @_);
    }

    sub get_name {
	my $self = shift;
	return $self->{NAME};
    }

    sub set_name {
	my $self = shift;
	$self->{NAME} = shift;
    }

    sub get_value {
	my $self = shift;
	return $self->{VALUE};
    }

    sub set_value {
	my $self = shift;
	$self->{RE} = undef;
	$self->{VALUE} = shift;
    }

    sub get_re {
	my $self = shift;
	if (! $self->{RE}) {
	    if (my $val = $self->get_value) {
		if ($val =~ m%^\w+$%) {
		    $self->{RE} = qr/\b\Q$val\E\b/;
		} else {
		    $self->{RE} = qr/\Q$val\E/;
		}
	    }
	}
	return $self->{RE};
    }

    sub explicit {
	my $self = shift;
	$self->{EXPLICIT} = shift if @_;
	return $self->{EXPLICIT};
    }
}

my $BaseVar = $opt{BaseVar} || 'SRCBASE';
my $BaseMacro = expand($BaseVar);
my $Indent = '  ';
my $Colon = ':';
my $Makefile;

# Returns the specified variable in a format to be expanded by make or
# any other parser (e.g. the shell) which understands ${FOO} expansion.
sub expand {
    return sprintf '${%s}', shift;
}

# Used to quote a word (typically a pathname) against makefile or
# similar syntax requirements. For readability, do this only as needed.
sub quote {
    my $word = shift;
    $word =~ s%([#])%\\$1%g;
    if ($word =~ m%\s%) {
	return qq("$word");
    } else {
	return $word;
    }
}

# Takes a path, returns a parameterized, quoted form.
sub parameterize {
    (my $path = shift) =~ s%^$BaseDir%$BaseMacro%o;
    return quote($path);
}

# Takes a parameterized, possibly quoted path and returns the original
# string form which can be opened or tested with -f.
sub deparameterize {
    ## TODO - figure out how to eval $BaseVar into the RE.
    (my $path = shift) =~ s%\${SRCBASE}%$BaseDir%;
    $path =~ s%"$%% if $path =~ s%^"%%;
    return $path;
}

# Takes a string and tries to substitute in known make variables such
# that "-c -g -O2 -Wall -DNDEBUG" might become "-c $(CFLAGS) $(DFLAGS)".
sub varify {
    my $text = shift;
    $text =~ s%\$%\$\$%g;
    for my $var (@{$Makefile->{VARLIST}}) {
	my $re = $var->get_re;
	my $name = $var->get_name;
	$text =~ s%$re%\$\{$name\}%g;
    }
    return $text;
}

# In order to fit make recipes into a CSV-line format we replaced
# interior newlines with a token of some kind, so before using
# the command we need to convert those tokens back into newlines,
# and add a tab for reasons of make syntax.
sub renew {
    my $line = "@_";
    $line =~ s%\^J\s*%\n\t%g;
    return $line;
}

# If the matched command was '/bin/sh -c "stuff"', what we want to
# put in the makefile is just "stuff" (without the quotes) because
# when make sees this it will run it as '/bin/sh -c "stuff"' again.
# The simplest way to drop a shell level is to actually run the
# shell in verbose (-v) and no-execute (-n) mode and capture the
# output. BTW it turns out that due to subtle variations and bugs
# in different Unix shells (bash, ksh, zsh, etc) it's better to
# always use /bin/sh for this purpose. Usually faster too. But
# there's an exception (boo): some newer Linux distros use 'dash'
# as their /bin/sh, and dash doesn't seem to implement -n or -v
# correctly, so we have to work around that. The current hack is
# that if /bin/sh == dash, use bash instead with some special flags.
# Argh.
sub desh {
    my $line = "@_";
    if ($broken_sh) {
	if ($line =~ s%^\S+sh\s+(-[a-z]*)c%/bin/bash --norc --noprofile ${1}nvc%) {
	    $line = qx($line 2>&1);
	}
    } else {
	if ($line =~ s%^\S+sh\s+(-[a-z]*)c%/bin/sh ${1}nvc%) {
	    $line = qx($line 2>&1);
	}
    }
    return $line;
}

# Prints a multi-line variable definition in a neatly formatted way.
sub printlist {
    my($name, @list) = @_;
    if (my $i = @list) {
	print MakeVar->new->assign($name, undef, undef)->format(" \\");
	for my $pa (@list) {
	    print $Indent, parameterize($pa->get_path);
	    print " \\" if --$i;
	    print "\n";
	}
	print "\n";
    }
}

# Some stuff needs to be at the top of a full Makefile.
if ($opt{full}) {
    push(@{$Makefile->{HEADER}}, ".PHONY: all clean rt\n\n");
    push(@{$Makefile->{HEADER}}, ".SUFFIXES:\n\n");
    push(@{$Makefile->{HEADER}}, ".ONESHELL:\n\n") unless $opt{NOGNU};
    push(@{$Makefile->{HEADER}}, "all:\n\n");
}

# Generate a list of macro substitutions automatically from the output
# of GNU make's -p flag.
if ($opt{pdump}) {
    die "$prog: Error: 'pdump' feature only supported for GNU make\n"
	if $opt{NOGNU};
    # Convenience feature - dump the make database automatically.
    # Note that we cannot check the exit status of this because 
    # in a recursive-make scenario it may return errors due to 
    # documented -q semantics. So we just work with whatever
    # variables it dumped whether it nominally succeeded or not.
    my $pdump = File::Spec->join($BaseDir, 'PDUMP');
    my $cmd = "set -x; make -r -q -p >$pdump 2>/dev/null";
    system($cmd);

    my %redundant;
    open(PDUMP, $pdump) || syserr(1, $pdump);
    for (my $prev; <PDUMP>; $prev = $_) {
	chomp;
	# If the old build model is recursive there may be multiple
	# "Variables" sections. Throw them all out except the last.
	if (m%^# Variables$%) {
	    @{$Makefile->{VARLIST}} = ();
	    %redundant = ();
	}

	# Variables are marked as 'makefile', 'default, or 'automatic'.
	# We don't want the automatic. Some of the default vars are useful
	# but some are not, and default vars should never be explicitly
	# assigned in the new makefile so we need to mark them as such.
	next unless $prev && $prev =~ m%^# (makefile .from.*|default)%;
	my $provenance = $1;
	my $var = MakeVar->new($_);
	$var->explicit(0) if $provenance =~ m%default%;

	if (!$var->get_value || length($var->get_value) < 2) {
	    next;
	}

	# We choose to ignore all vars whose names contain special
	# characters. This eliminates a lot of deliberately hidden
	# variables as well as possibly a few useful ones.
	next if $var->get_name !~ m%^\w+$%;

	# Similarly, assume these are hidden for a reason.
	next if $var->get_name =~ m%^_%;

	# These come up a lot in automake-generated makefiles and are
	# not intended to be user visible.
	next if $var->get_name =~ m%^am__%;

	# A bunch of special cases, mostly generated values which change
	# unreliably. Also some that are just not of much use.
	next if $var->get_name =~ m%^(?:CURDIR|MAKEFILE_LIST|MAKEFLAGS|MAKEFILES|MAKE_VERSION|CWEAVE|WEAVE|CTANGLE|TANGLE|OUTPUT_OPTION|F77|CO|GET|AS|PC|M2C|COFLAGS|SUFFIXES|OBJC|F77FLAGS)$%;

	# The $(MAKE) variable is handled specially.
	if ($var->get_name eq 'MAKE') {
	    next;
	} elsif ($var->get_name eq 'MAKE_COMMAND') {
	    $var->set_name('MAKE');
	}

	next if $redundant{$var->get_re}++;
	push(@{$Makefile->{VARLIST}}, $var);
    }
    close PDUMP;
    unlink($pdump);
}

# If a file containing explicit macro definitions is provided, store
# them in the same place.
if (my $vars = $opt{vars}) {
    my %redundant;
    open(VARS, $vars) || syserr(1, $vars);
    for (<VARS>) {
	my $var = MakeVar->new($_);
	next unless $var->get_value;
	next if $redundant{$var->get_re}++;
	push(@{$Makefile->{VARLIST}}, $var);
    }
    close VARS;
}

# This is a special variable which must always be available.
my $basevar = MakeVar->new->assign($BaseVar, ':=', $BaseDir);
unshift(@{$Makefile->{VARLIST}}, $basevar);

# Derive the set of required "rules" (target: prereq sequences
# optionally followed by recipes).
my %Duplicated;
for my $ca (@Actions) {
    my(%tgtpaths, %prereqs);
    my @targets = $ca->get_targets;
    next unless @targets;

    for (my $i = 0; $i <= $#targets; $i++) {
	my $target = $targets[$i];
	my $path = $target->get_path;

	# See if any targets are duplicated. If so, this would likely
	# reflect a bug in the original build model but we have to
	# deal with it.  Currently this is done by converting to use
	# of double-colon rules exclusively whenever duplications
	# exist.  It's hard to generate a combination of single- and
	# double-colon rules reliably so we go all single (preferred)
	# or all double.
	$Colon = '::' if $Duplicated{$path}++ && $ca->get_prereqs;

	if (!exists($DirTargets{$path})) {
	    $FileTargets{$path} = $target;
	    # Special-case hack: when we see the command "ln -s foo bar"
	    # we pretend that it was necessary to open the link target "foo"
	    # in order to create the symlink "bar". This is not literally
	    # true but in practice a makefile should show such a dependency.
	    if ($target->is_symlink) {
		my $path2 = $target->get_path2;
		if (! $opt{'existing-only'} || -e $path2) {
		    $prereqs{$path2}++;
		}
	    }
	}

	# Make sure no file mentioned as a target shows up also as a prereq.
	$tgtpaths{$path}++;
    }

    # In order to enable parallelism we show only the primary
    # target with the recipe, while sibling targets are made to
    # depend on the primary sans recipe. If multiple targets
    # are listed to the left of a colon they will compete
    # with each other in parallel mode due to make semantics.

    my $recipe_key = undef;

    if ($opt{Parallel}) {
	my $primary = shift @targets;
	$recipe_key = parameterize($primary->get_path);
	if (@targets) {
	    my @tlist;
	    for my $target (@targets) {
		push(@tlist, parameterize($target->get_path));
	    }

	    my $tkey = join(':', sort @tlist);
	    $Makefile->{RULES}->{$tkey}->{PREREQS}->{$recipe_key}++;
	}
    } else {
	my @tlist;
	for my $target (@targets) {
	    push(@tlist, parameterize($target->get_path));
	}
	$recipe_key = join(':', @tlist);
    }

    # Add the standard set of prereqs to the "derived" list.
    for my $prq ($ca->get_prereqs) {
	my $prq_path = $prq->get_path;
	$prereqs{$prq_path}++;
    }

    my @prqpaths = sort
	grep { !exists($tgtpaths{$_}) && !exists($DirTargets{$_}) }
	keys %prereqs;
    for my $prq (@prqpaths) {
	my $pprq = parameterize($prq);
	$Makefile->{RULES}->{$recipe_key}->{PREREQS}->{$pprq}++;
    }

    # Derive the command line (aka recipe) if it's going to be printed.
    # Note: there used to be some code here to swap in $@ for the
    # actual target path but that turns out to be harder than expected
    # so for now at least it's out.
    if ($opt{full}) {
	$Makefile->{RULES}->{$recipe_key}->{RWD} = $ca->get_rwd;
	my $line = $ca->get_line;
	$line = renew($line);
	$line =~ s%^\s+%%;
	$line =~ s%\s+$%%;
	$line =~ s%[ ]{2,}% %g unless $line =~ m%["'\\]%;
	$line = desh($line) unless MSWIN;
	$line = varify($line);
	$Makefile->{RULES}->{$recipe_key}->{RECIPE} = $line;
    }
}

###########################################################################
# At this point we've constructed a makefile abstraction in a hash.
# From here on we're concerned only with formatting and printing it.
###########################################################################

my %Includes;

my $ofile = $opt{'output-file'};
if ($ofile && $ofile ne '-') {
    if ($iext && open(OUTPUT, $ofile)) {
	for (<OUTPUT>) {
	    chomp;
	    if (m%^-?include\s+(.*)%) {
		my $incfile = deparameterize($1);
		$Includes{$incfile}++ if -f $incfile;
	    }
	}
	close(OUTPUT);
    }
    unlink $ofile;
    open(STDOUT, '>', $ofile) || syserr(1, $ofile);
}

if ($ofile) {
    for my $line (@{$Makefile->{HEADER}}) {
	print $line;
    }

    # Dump the list of printable (non-default) make variables.
    for my $var (@{$Makefile->{VARLIST}}) {
	next unless $var->explicit;
	print $var->format;
    }
}

# Dump the set of rules and their recipes if present.
for my $tkey (sort keys %{$Makefile->{RULES}}) {

    my @tgtlist = split ':', $tkey;
    my @prqlist = sort keys %{$Makefile->{RULES}->{$tkey}->{PREREQS}};

    my $fh;
    if ($iext) {
	my $incfile = join('.', deparameterize($tgtlist[0]), $iext);
	open(INCFILE, '>', $incfile) || die "$prog: Error: $incfile: $!\n";
	$fh = select INCFILE;
	$Includes{$incfile}++;
	if ($opt{dbg}) {
	    print q($(info + Including $(lastword $(MAKEFILE_LIST)))), "\n\n";
	}
	print $basevar->format;
    }

    print "\n";

    for (my $i = 0; $i <= $#tgtlist; $i++) {
	print $tgtlist[$i];
	if ($i < $#tgtlist) {
	    print " \\";
	} else {
	    print $Colon;
	    print " \\" if @prqlist;
	}
	print "\n";
    }

    for (my $i = 0; $i <= $#prqlist; $i++) {
	print $Indent, $prqlist[$i];
	print " \\" if $i < $#prqlist;
	print "\n";
    }

    if (my $recipe = $Makefile->{RULES}->{$tkey}->{RECIPE}) {
	my $rwd = $Makefile->{RULES}->{$tkey}->{RWD};
	my $dir = $BaseMacro;
	$dir = File::Spec->join($dir, $rwd) if $rwd && $rwd ne '.';
	$dir = File::Spec->canonpath($dir);
	$dir = quote($dir);
	printf "\tcd %s && %s\n", $dir, $recipe;
    }

    if ($iext) {
	select($fh);
	close(INCFILE);
    }
}

if ($ofile && $iext) {
    print "\n";
    for my $incfile (sort keys %Includes) {
	my $pinc = parameterize($incfile);
	print "-include $pinc\n";
    }
}

# Flesh out the rest of the full makefile if requested.
if ($ofile && $opt{full}) {

    print "\n";

    for my $dir (keys %DirTargets) {
	my $parent = dirname($dir);
	next unless exists $DirTargets{$parent};
	printf "%s$Colon \\\n$Indent%s\n\n",
	    parameterize($dir), parameterize($parent);
    }

    printlist('FILE_TARGETS',
	sort { $a->get_path cmp $b->get_path } values %FileTargets);

    # We need special handling of symlinks because of a subtlety.
    # Consider the sequence (in two separate recipes):
    # 		ln -s foo/ bar
    # 		cp bar/zar zar2
    # Since we only keep track of _real_ paths, the dependency
    # of zar2 on the *path* bar/zar is not recorded, though the
    # dependency on the *file* is. Therefore make does not know
    # that the link recipe must run before the cp. The best
    # solution I've come up with so far is to make all other
    # targets dependent on all symlinks.
    printlist('DIR_LINK_TARGETS',
	sort { $a->get_path cmp $b->get_path } values %DirLinkTargets);

    # These are sorted in reverse order so that rmdir can be used
    # in the "clean" target.
    my @dirlist;
    for my $pa (sort { $b->get_path cmp $a->get_path } values %DirTargets) {
	my $path = $pa->get_path;
	push(@dirlist, $pa) if !exists($DirLinkTargets{$path});
    }
    printlist('DIR_TARGETS', @dirlist);

    # The ordering of targets here is meaningful, as directories must
    # exist before files can be put in them. We try to handle it
    # correctly with explicit dependencies but the default ordering
    # implied here can't hurt.
    my $tgtvars = '';
    $tgtvars .= ' ' . expand('DIR_LINK_TARGETS') if %DirLinkTargets;
    $tgtvars .= ' ' . expand('DIR_TARGETS') if %DirTargets;
    $tgtvars .= ' ' . expand('FILE_TARGETS') if %FileTargets;
    if ($tgtvars) {
	print MakeVar->new->assign('ALL_TARGETS', undef, $tgtvars)->format;
	printf "\nall: %s\n\n", expand('ALL_TARGETS');
    }

    if (%DirLinkTargets && %FileTargets) {
	printf "%s: %s\n\n", expand('FILE_TARGETS'), expand('DIR_LINK_TARGETS');
    }

    printf "clean:\n\t%s\n", '@echo Cleaning all targets ...';
    $tgtvars = '';
    $tgtvars .= ' ' . expand('DIR_LINK_TARGETS') if %DirLinkTargets;
    $tgtvars .= ' ' . expand('FILE_TARGETS') if %FileTargets;
    $tgtvars =~ s%^\s+%%;
    if ($tgtvars) {
	printf "\t\$(RM) %s\n", $tgtvars;
    }
    if (%DirTargets) {
	printf "\t\$(RM) -r %s\n", expand('DIR_TARGETS');
    }

    printf "\nrt: clean all\n";
}

__END__

=head1 NAME

ao2make - Generate a Makefile from AO audit data

=head1 OPTIONS

   -help		Print this message and exit
   -base dir		Specify the root dir (default: cwd)
   -ext string		The extension for prereq fragment files (default: .d)
   -full		Generate a useable makefile (default: dependencies only)
   -members-only	Ignore files which are not project members
   -output file		Create a Makefile in the specified file
   -pdump 		(with -full) Use variables extracted from GNU "make -p"
   -vars file		(with -full) A set of make variables to be substituted

=head2 NOTES

    All flags may be abbreviated to their shortest unique name.
    Input may be read from standard input or a named audit-file

=head2 EXAMPLES

    ao2make audit-file
    ao2make -o Makefile < audit-file
    ao2make -o Makefile -full -pdump < audit-file

=head1 DESCRIPTION

The script can generate make-format dependency data, or even a full working
makefile, from the output of the AO auditing tool. By default it will
create a set of I<target.d> files, one for each target. In the case of
sibling groups it will guess at a I<primary> target and use that for the
name of the prereq file.

With the I<-output> flag it will I<also> produce a wrapper Makefile
which includes all the prereq files. If I<-full> is added,
the generated file will be a full, self-contained Makefile
which inlines the prereq data and adds build commands, aka "recipes".

It can be used in a few ways:

=over 4

=item * DEPENDENCY DATA

The simplest use is to add full prerequisite information to an
existing, handcrafted Makefile suite. Simply add an I<include>
statement to the Makefile and use this to generate the include
file. Now the Makefile will have full knowledge of which files
depend on which other files, making the build more reliable and
also opening the way to parallel builds, since generally the
problems of parallelism are solved by full dependency information.
This mode has no required flags, though you can change the file
extension with I<-extension> and may need I<-base> with recursive
build models.

=item * COMPLETE MAKEFILE

A more ambitious use is to generate a full, working Makefile from
an old-style build. If the old build model is creaky and unreliable
and/or recursive, this can be used to generate a single, flat
Makefile with full prerequisite data as above. After some
massaging, the new Makefile may be able to replace the old, and
thus this can be used as an automated way to update old build
tooling. Use I<-full> and optionally I<-pdump> or I<-vars>.

=item * OTHER FORMATS

This script could be modified to generate any other format. The
majority of the code simply reads audit data into a set of Perl
hashes; this data can be sliced and diced and dumped in any format,
limited only by your Perl skills.

=back

=head1 COPYRIGHT

Copyright (c) 2005-2011 David Boyce.  All rights reserved.

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
