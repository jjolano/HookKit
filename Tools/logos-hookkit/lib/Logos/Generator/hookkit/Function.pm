package Logos::Generator::hookkit::Function;
use strict;
use parent qw(Logos::Generator::Base::Function);

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
    my $r = "{ ";
    $r .= "(void)_hk_hookkit_hook_function((void*)(".$sym."), (void*)&".$self->newFunctionName($function).", (void**)&".$self->originalFunctionName($function).");";
    $r .= " }";
    return $r;
}

1;
