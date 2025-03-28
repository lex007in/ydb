/* syntax version 1 */

pragma UseBlocks;

SELECT
    value,
    Unicode::ToLower(value) AS lower,
    Unicode::ToUpper(value) AS upper,
    Unicode::ToTitle(value) AS title,
    Unicode::Reverse(value) AS reverse,
FROM Input;

