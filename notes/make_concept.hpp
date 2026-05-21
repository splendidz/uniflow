#include <vector>
#include <string>
#include <unordered_map>
class StepWiseBase;

class StepWiseFactory
{
    StepWiseFactory() = default;
    std::vector<StepWiseBase*> objs;
public:
    // singleton
    StepWiseFactory& GetInst()
    {
        static StepWiseFactory obj;
        return obj;
    }
    
    void ExecuteSteps()
    {
        for (auto &o : objs)    
        {    
            if(o->GetStep() != 0)
                o->NextStep();
        }
    }

};

class StepWiseBase
{
private:
    int _curr_step = 0;
    std::unordered_map<std::string, int> step_definitions;
public:
    StepWiseBase() = default;
    
    int GetEntryStepNo(std::string step_name) // 문제시 -1
    {
        auto it =  step_definitions.find(step_name); // 예외 처리 필요
        return it->second; 
    }
    void NextStep() {_curr_step++;} // 이 함수는 상속 못하게 막고 싶음
    int GetStep(){return _curr_step;} // 이 함수는 상속 못하게 막고 싶음
    void ResetStep() {_curr_step = 0;} // 이 함수는 상속 못하게 막고 싶음
    virtual void ExecuteStep() = 0;
    bool EnterStep(std::string step_name) // 지금은 return bool 인데 에러 코드에 대한 정의도 필요하다. (음수로)
    {
        if(_curr_step != 0)
            return false;

        _curr_step = GetEntryStepNo(step_name);  // 스텝을 넣어준다    
    }
    void StepDefinition(std::string step_name, int start_step) 
    {
        //step_definitions에 추가한다. 이름이 겹치거나 step의 범위가 겸치면 assert 친다.
        // step은 0보다 커야하고 범위가 최대 100이상을 넘을 수 없다. 보통 100, 200, 300 이런 식으로 할당되기 예상한다.
    }
};



 

class StepWiseMessageHandler : StepWiseBase
{

public:

    StepWiseMessageHandler()
    {
        this->StepDefinition("ProcessMessage", 100);  // ProcessMessage라는 역할을 하는 스텝이 100 ~ 200(199까지) 할당된다.
    }
    std::pair<std::string, int>* STEP_DEFINITIONS()
    {
        static std::pair<std::string, int> STEP_DEFINES[] = {std::make_pair("ProcessMessage", 100) };  
        return STEP_DEFINES;
    }

    void ExecuteStep() override
    {
        auto cur_step = this->GetStep();
        switch(cur_step)
        {
            case 0: break;
            case 100:
            
            break;
                
        }
    }

};

class StepWiseJobEventHandler : StepWiseBase
{
public:
    StepWiseJobEventHandler() = default;
    void ExecuteStep() override
    {
        auto cur_step = this->GetStep();
        switch(cur_step)
        {
            case 0: break;
            case 100:
            
            break;
                
        }
    }

};