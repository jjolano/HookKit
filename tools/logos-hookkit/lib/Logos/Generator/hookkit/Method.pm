package Logos::Generator::hookkit::Method;
use strict;
use parent qw(Logos::Generator::internal::Method);
use Digest::MD5 'md5_hex';

sub _hk_stable_hook_id {
    my ($class, $selector) = @_;
    $class //= ""; $selector //= "";
    # Fallback old literal if names absent (prevents empty components)
    if ($class eq "" || $selector eq "") {
        return "logos.hook";
    }
    my $safe_class = $class; $safe_class =~ s/\./_/g; $safe_class =~ s/:/_/g; $safe_class =~ s/[^A-Za-z0-9_.\-]/_/g;
    my $safe_sel = $selector; $safe_sel =~ s/\./_/g; $safe_sel =~ s/:/_/g; $safe_sel =~ s/[^A-Za-z0-9_.\-]/_/g;
    my $base = "logos.hook.$safe_class.$safe_sel";
    $base =~ s/[^A-Za-z0-9_.\-]/_/g;
    my $hash = substr(md5_hex("$class:$selector"), 0, 8);
    my $id = "$base.$hash";
    if (length($id) >= 64) {
        my $keep = 63 - length($hash) - 1; # base + '.' + hash <64
        $base = substr($base, 0, $keep);
        $base =~ s/[._\-]+$//;
        $id = "$base.$hash";
        $id = substr($id, 0, 63) if length($id) >= 64;
    }
    return $id;
}

sub initializers {
    my $self = shift;
    my $method = shift;
    my $cgen = Logos::Generator::for($method->class);
    my $classvar = ($method->scope eq "+" ? $cgen->metaVariable : $cgen->variable);
    my $isMeta = ($method->scope eq "+" ? 1 : 0);
    # stable_hook_id: logos.hook.<Class>.<selector>.<8hex> (<64 chars), 8hex like %ctor
    my $class_name = eval { $method->class->name } // "";
    my $selector = eval { $method->selector } // "";
    my $stable_id = _hk_stable_hook_id($class_name, $selector);
    # escape for C string literal
    (my $stable_c = $stable_id) =~ s/\"/\\\"/g;
    my $r = "{ ";
    if (!$method->isNew) {
        # inheritance opt-in: per-method annotate gates global %config hook_inheritance
        my $cfg = $main::CONFIG{hook_inheritance} // "local_only";
        my $allow_global = ($cfg eq "allow_inherited");
        my $annotated = $method->{_hookkit_allow_inherited} ? 1 : 0;
        my $allow_inherited = ($allow_global && $annotated) ? 1 : 0;
        $r .= "(void)_hk_hookkit_hook_message(\"$stable_c\", ".$classvar.", ".$self->selectorRef($method->selector).", (IMP)&".$self->newFunctionName($method).", (IMP*)&".$self->originalFunctionName($method).", ".$isMeta.", ".$allow_inherited.");";
    } else {
        if (!$method->type) {
            $r .= "char _typeEncoding[1024]; unsigned int i = 0; ";
            for ($method->return, "id", "SEL", @{$method->argtypes}) {
                my $typeEncoding = Logos::Method::typeEncodingForArgType($_);
                if (defined $typeEncoding) {
                    my @typeEncodingBits = split(//, $typeEncoding);
                    my $i = 0;
                    for my $char (@typeEncodingBits) {
                        $r .= "_typeEncoding[i".($i > 0 ? " + $i" : "")."] = '$char'; ";
                        $i++;
                    }
                    $r .= "i += ".(scalar @typeEncodingBits)."; ";
                } else {
                    $r .= "memcpy(_typeEncoding + i, \@encode($_), strlen(\@encode($_))); i += strlen(\@encode($_)); ";
                }
            }
            $r .= "_typeEncoding[i] = '\\0'; ";
        } else {
            $r .= "const char *_typeEncoding = \"".$method->type."\"; ";
        }
        $r .= "class_addMethod(".$classvar.", ".$self->selectorRef($method->selector).", (IMP)&".$self->newFunctionName($method).", _typeEncoding); ";
    }
    $r .= "}";
    return $r;
}

1;
