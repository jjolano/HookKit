package Logos::Generator::hookkit::Method;
use strict;
use parent qw(Logos::Generator::internal::Method);

sub initializers {
    my $self = shift;
    my $method = shift;
    my $cgen = Logos::Generator::for($method->class);
    my $classvar = ($method->scope eq "+" ? $cgen->metaVariable : $cgen->variable);
    my $isMeta = ($method->scope eq "+" ? 1 : 0);
    my $r = "{ ";
    if (!$method->isNew) {
        $r .= "(void)_hk_hookkit_hook_message(".$classvar.", ".$self->selectorRef($method->selector).", (IMP)&".$self->newFunctionName($method).", (IMP*)&".$self->originalFunctionName($method).", ".$isMeta.");";
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
