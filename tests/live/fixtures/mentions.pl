use Irssi;
our %IRSSI = (name => 'mentions');
Irssi::command_bind('mentions', sub { Irssi::print('OLD SCRIPT RAN') });
