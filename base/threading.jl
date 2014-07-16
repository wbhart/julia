module Threading

export @parblock, @parfun


macro parblock(args...)
    na = length(args)
    if na != 1
        throw(ArgumentError("wrong number of arguments in @parblock"))
    end
    blk = args[1]
    if !isa(blk, Expr) || !is(blk.head, :block)
        throw(ArgumentError("argument to @parblock must be a block"))
    end
    fn = gensym("parf")
    tid = symbol("tid")
    quote
        function $fn(t::Int16)
            local $(esc(tid)) = t
            $(esc(blk))
        end
        work = ccall(:jl_threading_prepare_work, Ptr{Void}, (Any, Any),
                     $fn, (convert(Int16, 0),))
        gc_disable()
        ccall(:jl_threading_do_work, Void, (Ptr{Void},), work)
        gc_enable()
    end
end

macro parfun(args...)
    na = length(args)
    if na < 1
        throw(ArgumentError("wrong number of arguments in @parallel_fun"))
    end
    fun = args[1]
    if !isa(fun, Function)
        throw(ArgumentError("argument to @parallel_fun must be a function"))
    end
    quote
        work = ccall(:jl_threading_prepare_work, Ptr{Void}, (Any, Any),
                     $fun, args[2:end])
        gc_disable()
        ccall(:jl_threading_do_work, Void, (Ptr{Void},), work)
        gc_enable()
    end
end

end # module
