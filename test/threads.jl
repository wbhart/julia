using Base.Test
using Base.Threading

expd = zeros(Int16, Threading.num_threads)
for i = 1:Threading.num_threads
    expd[i] = Threading.num_threads - i + 1
end

arr = zeros(Int16, Threading.num_threads)
#println(arr)

function foo(A)
    @parallel_all begin
        A[tid] = 17 - tid
    end
end

function bar(baz)
    @parallel_all begin
        baz = baz + 1
    end
    return baz
end

@parallel_start

foo(arr)
@test arr == expd

baz = 0
baz = bar(baz)

@parallel_stop

println(baz)

