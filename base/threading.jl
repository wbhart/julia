module Threading

export threadid, @parblock, @parfun

threadid() = ccall(:jl_threadid, Int16, ())

macro parblock(args...)
    na = length(args)
    if na != 1
        throw(ArgumentError("wrong number of arguments in @parblock"))
    end
    blk = args[1]
    if !isa(blk, Expr) || !is(blk.head, :block)
        throw(ArgumentError("argument to @parblock must be a block"))
    end
    fun = gensym("parf")
    quote
        function $fun()
            $(esc(blk))
        end
        ccall(:jl_threading_run, Void, (Any, Any), $fun, ())
    end
end

macro parfun(args...)
    na = length(args)
    if na < 1
        throw(ArgumentError("wrong number of arguments in @parfun"))
    end
    fun = args[1]
    if !isa(fun, Function)
        throw(ArgumentError("argument to @parfun must be a function"))
    end
    quote
        ccall(:jl_threading_run, Void, (Any, Any), $fun, args[2:end])
    end
end

end # module

