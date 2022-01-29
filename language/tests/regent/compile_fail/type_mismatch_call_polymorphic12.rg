-- Copyright 2022 Stanford University
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- fails-with:
-- type_mismatch_call_polymorphic12.rg:50: type mismatch: expected double for field b but got int32
--   f(r.{d=w.{b=y}, d=z.{a=x}})
--    ^

import "regent"

struct vec2
{
  x : double;
  y : int;
}

struct fs
{
  z : vec2;
  w : vec2;
}

struct iface1
{
  a : double;
  b : double;
}

struct iface2
{
  d : iface1;
}

task f(x : region(iface2))
where reads writes(x) do end

task g()
  var r = region(ispace(ptr, 5), fs)
  f(r.{d=w.{b=y}, d=z.{a=x}})
end
