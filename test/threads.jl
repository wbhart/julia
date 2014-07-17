using Base.Test
using Base.Threading

# test 1
expected = [1:nthreads()]

arr = zeros(Int16, nthreads())

function foo(A)
    @parblock begin
	tid = threadid()
        A[tid] = tid
    end
end

foo(arr)

@show arr

@test arr == expected


# test 2 (from tknopp)

