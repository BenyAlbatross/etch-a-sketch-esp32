import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.parametrize("target", ["esp32s2"], indirect=True)
def test_unity(dut: IdfDut) -> None:
    dut.run_all_single_board_cases()
