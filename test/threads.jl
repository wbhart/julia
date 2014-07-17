using Base.Test
using Base.Threading

nthreads = maxthreads()

# test 1
expected = [nthreads - i + 1 for i in 1:nthreads]

arr = zeros(Int16, nthreads)

function foo(A)
    @parblock begin
	tid = threadid()
        A[tid] = 17 - tid
    end
end

foo(arr)
@test arr == expected


# test 2 (from tknopp)

