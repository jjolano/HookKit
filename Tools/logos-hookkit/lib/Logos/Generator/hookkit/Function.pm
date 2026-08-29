package Logos::Generator::hookkit::Function;
use strict;
use parent qw(Logos::Generator::Base::Function);
use Digest::MD5 'md5_hex';

sub _hk_stable_func_id {
    my ($sym) = @_;
    $sym //= "";
    $sym =~ s/^\s+//; $sym =~ s/\s+$//;
    if ($sym eq "") {
        return "logos.func";
    }
    # sanitize: .→_, :→_, and any non alnum _ . - → _
    my $safe = $sym;
    $safe =~ s/^\s*\(void\s*\*\s*\)\s*//; # strip cast if present after trimming
    $safe =~ s/^\s*&\s*//; # strip address-of
    $safe =~ s/\./_/g; $safe =~ s/:/_/g; $safe =~ s/[^A-Za-z0-9_.\-]/_/g;
    $safe =~ s/__+/_/g; $safe =~ s/^_+//; $safe =~ s/_+$//;
    $safe = "sym" if $safe eq "";
    my $base = "logos.func.$safe";
    $base =~ s/[^A-Za-z0-9_.\-]/_/g;
    my $hash = substr(md5_hex($sym), 0, 8);
    my $id = "$base.$hash";
    if (length($id) >= 64) {
        my $keep = 63 - length($hash) - 1;
        $base = substr($base, 0, $keep);
        $base =~ s/[._\-]+$//;
        $id = "$base.$hash";
        $id = substr($id, 0, 63) if length($id) >= 64;
    }
    return $id;
}

sub initializers {
    my $self = shift;
    my $function = shift;
    # %hookf / %log hooks via HookKit function address
    # Base::Function::initializers is not implemented normally — we hook via symbol address
    # Function name is available via _initExpression: (void*)funcName or expression
    my $expr = $self->_initExpression($function);
    # strip (void*) cast for HookKit
    my $sym = $expr;
    $sym =~ s/^\(void\s*\*\)//;
    $sym =~ s/^\s*//;
    my $stable_id = _hk_stable_func_id($sym);
    (my $stable_c = $stable_id) =~ s/\"/\\\"/g;
    my $r = "{ ";
    $r .= "(void)_hk_hookkit_hook_function(\"$stable_c\", (void*)(".$sym."), (void*)&".$self->newFunctionName($function).", (void**)&".$self->originalFunctionName($function).");";
    $r .= " }";
    return $r;
}

1;
