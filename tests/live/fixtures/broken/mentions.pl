use Irssi;
open(my $fh, '>', "$ENV{HOME}/broken.ran") and close($fh);
die "BROKEN FIXTURE";
